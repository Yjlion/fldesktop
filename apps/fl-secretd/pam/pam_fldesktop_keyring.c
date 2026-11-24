/* pam_fldesktop_keyring: unlocks fldesktop's login keyring (fl-secretd) with
 * the login password, and changes its password with the account's.
 *
 *   auth      optional pam_fldesktop_keyring.so      keeps the password
 *   session   optional pam_fldesktop_keyring.so      unlocks (after pam_systemd)
 *   password  optional pam_fldesktop_keyring.so      re-encrypts on a change
 *
 * The keyring work runs as the user in "fl-secretd --login" (the password on
 * a pipe); this module never touches the user's files as root. It never
 * fails a login: every error is only logged. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <syslog.h>
#include <unistd.h>

#include <security/pam_ext.h>
#include <security/pam_modules.h>

#ifndef FLD_SECRETD
#define FLD_SECRETD "/usr/bin/fl-secretd"
#endif
#define DATA_KEY "fldesktop-keyring-password"

static void wipe_free(pam_handle_t *pamh, void *data, int status) {
  (void)pamh;
  (void)status;
  if (data) {
    explicit_bzero(data, strlen((char *)data));
    free(data);
  }
}

/* runs fl-secretd <arg> as the user with input on its stdin */
static int run_helper(pam_handle_t *pamh, const char *arg, const char *input) {
  const char *user = NULL;
  if (pam_get_user(pamh, &user, NULL) != PAM_SUCCESS || !user) return -1;
  struct passwd *pw = getpwnam(user);
  if (!pw) return -1;
  if (access(FLD_SECRETD, X_OK) != 0) return -1;
  int p[2];
  if (pipe2(p, O_CLOEXEC) != 0) return -1;
  /* the child's environment: the session's runtime directory */
  const char *rt = pam_getenv(pamh, "XDG_RUNTIME_DIR");
  struct sigaction sa, old;
  memset(&sa, 0, sizeof sa);
  sa.sa_handler = SIG_DFL;
  sigaction(SIGCHLD, &sa, &old);
  pid_t pid = fork();
  if (pid < 0) {
    close(p[0]);
    close(p[1]);
    sigaction(SIGCHLD, &old, NULL);
    return -1;
  }
  if (pid == 0) {
    dup2(p[0], 0);
    if (setgid(pw->pw_gid) != 0 || initgroups(pw->pw_name, pw->pw_gid) != 0 || setuid(pw->pw_uid) != 0 ||
        setuid(0) == 0)
      _exit(126);
    char home[1024], rundir[512], userenv[300];
    snprintf(home, sizeof home, "HOME=%s", pw->pw_dir);
    snprintf(userenv, sizeof userenv, "USER=%s", pw->pw_name);
    char run_default[64];
    snprintf(run_default, sizeof run_default, "/run/user/%u", (unsigned)pw->pw_uid);
    snprintf(rundir, sizeof rundir, "XDG_RUNTIME_DIR=%s", rt && *rt ? rt : run_default);
    char *envp[] = {home, userenv, rundir, "PATH=/usr/bin:/bin", NULL};
    if (chdir(pw->pw_dir) != 0 && chdir("/") != 0) _exit(126);
    execle(FLD_SECRETD, "fl-secretd", arg, (char *)NULL, envp);
    _exit(127);
  }
  close(p[0]);
  size_t len = strlen(input), off = 0;
  while (off < len) {
    ssize_t n = write(p[1], input + off, len - off);
    if (n <= 0) break;
    off += (size_t)n;
  }
  close(p[1]);
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  sigaction(SIGCHLD, &old, NULL);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

PAM_EXTERN int pam_sm_authenticate(pam_handle_t *pamh, int flags, int argc, const char **argv) {
  (void)flags;
  (void)argc;
  (void)argv;
  const void *tok = NULL;
  if (pam_get_item(pamh, PAM_AUTHTOK, &tok) == PAM_SUCCESS && tok) {
    char *copy = strdup((const char *)tok);
    if (copy && pam_set_data(pamh, DATA_KEY, copy, wipe_free) != PAM_SUCCESS) wipe_free(pamh, copy, 0);
  }
  return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_setcred(pam_handle_t *pamh, int flags, int argc, const char **argv) {
  (void)pamh;
  (void)flags;
  (void)argc;
  (void)argv;
  return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_open_session(pam_handle_t *pamh, int flags, int argc, const char **argv) {
  (void)flags;
  (void)argc;
  (void)argv;
  const void *pw = NULL;
  if (pam_get_data(pamh, DATA_KEY, &pw) != PAM_SUCCESS || !pw) return PAM_IGNORE; /* autologin: asked later */
  size_t n = strlen((const char *)pw);
  char *line = malloc(n + 2);
  if (!line) return PAM_IGNORE;
  memcpy(line, pw, n);
  line[n] = '\n';
  line[n + 1] = '\0';
  int rc = run_helper(pamh, "--login", line);
  explicit_bzero(line, n + 1);
  free(line);
  pam_set_data(pamh, DATA_KEY, NULL, NULL); /* forget it now (wipe_free runs) */
  if (rc != 0) pam_syslog(pamh, LOG_NOTICE, "fl-secretd --login failed (%d)", rc);
  return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_close_session(pam_handle_t *pamh, int flags, int argc, const char **argv) {
  (void)pamh;
  (void)flags;
  (void)argc;
  (void)argv;
  return PAM_IGNORE;
}

PAM_EXTERN int pam_sm_chauthtok(pam_handle_t *pamh, int flags, int argc, const char **argv) {
  (void)argc;
  (void)argv;
  if (!(flags & PAM_UPDATE_AUTHTOK)) return PAM_IGNORE;
  const void *old = NULL, *new_ = NULL;
  pam_get_item(pamh, PAM_OLDAUTHTOK, &old);
  pam_get_item(pamh, PAM_AUTHTOK, &new_);
  if (!old || !new_) return PAM_IGNORE;
  size_t a = strlen((const char *)old), b = strlen((const char *)new_);
  char *both = malloc(a + b + 3);
  if (!both) return PAM_IGNORE;
  snprintf(both, a + b + 3, "%s\n%s\n", (const char *)old, (const char *)new_);
  int rc = run_helper(pamh, "--change-login", both);
  explicit_bzero(both, a + b + 2);
  free(both);
  if (rc != 0) pam_syslog(pamh, LOG_NOTICE, "the login keyring keeps its old password (%d)", rc);
  return PAM_IGNORE;
}
