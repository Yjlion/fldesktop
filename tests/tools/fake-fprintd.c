/* A fake fprintd for fprint-mock.sh, on the session bus: one reader,
 * "right-index-finger" enrolled for everyone; verifying gives a mismatch,
 * then a match; enrolling passes three stages. */
#include <gio/gio.h>
#include <string.h>

static const char *xml =
	"<node>"
	" <interface name='net.reactivated.Fprint.Manager'>"
	"  <method name='GetDefaultDevice'><arg type='o' direction='out'/></method>"
	" </interface>"
	" <interface name='net.reactivated.Fprint.Device'>"
	"  <method name='Claim'><arg type='s' direction='in'/></method>"
	"  <method name='Release'/>"
	"  <method name='ListEnrolledFingers'><arg type='s' direction='in'/><arg type='as' direction='out'/></method>"
	"  <method name='VerifyStart'><arg type='s' direction='in'/></method>"
	"  <method name='VerifyStop'/>"
	"  <method name='EnrollStart'><arg type='s' direction='in'/></method>"
	"  <method name='EnrollStop'/>"
	"  <method name='DeleteEnrolledFingers2'/>"
	"  <signal name='VerifyStatus'><arg type='s'/><arg type='b'/></signal>"
	"  <signal name='EnrollStatus'><arg type='s'/><arg type='b'/></signal>"
	"  <property name='num-enroll-stages' type='i' access='read'/>"
	" </interface>"
	"</node>";
#define DEV "/net/reactivated/Fprint/Device/0"
static int verifies;

struct emit { GDBusConnection *c; const char *sig, *result; gboolean done; };
static gboolean do_emit(gpointer p) {
	struct emit *e = p;
	g_dbus_connection_emit_signal(e->c, NULL, DEV, "net.reactivated.Fprint.Device", e->sig,
		g_variant_new("(sb)", e->result, e->done), NULL);
	g_free(e);
	return G_SOURCE_REMOVE;
}
static void emit_later(GDBusConnection *c, const char *sig, const char *result, gboolean done, int ms) {
	struct emit *e = g_new(struct emit, 1);
	*e = (struct emit){c, sig, result, done};
	g_timeout_add(ms, do_emit, e);
}

static void call(GDBusConnection *c, const char *sender, const char *path, const char *iface, const char *method,
		GVariant *params, GDBusMethodInvocation *inv, gpointer data) {
	if (!strcmp(method, "GetDefaultDevice")) {
		g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", DEV));
	} else if (!strcmp(method, "ListEnrolledFingers")) {
		const char *f[] = {"right-index-finger", NULL};
		g_dbus_method_invocation_return_value(inv, g_variant_new("(^as)", f));
	} else if (!strcmp(method, "VerifyStart")) {
		verifies++;
		g_dbus_method_invocation_return_value(inv, NULL);
		emit_later(c, "VerifyStatus", verifies == 1 ? "verify-no-match" : "verify-match", TRUE, 200);
	} else if (!strcmp(method, "EnrollStart")) {
		g_dbus_method_invocation_return_value(inv, NULL);
		emit_later(c, "EnrollStatus", "enroll-stage-passed", FALSE, 100);
		emit_later(c, "EnrollStatus", "enroll-stage-passed", FALSE, 200);
		emit_later(c, "EnrollStatus", "enroll-completed", TRUE, 300);
	} else {
		g_dbus_method_invocation_return_value(inv, NULL);
	}
}

static GVariant *get_prop(GDBusConnection *c, const char *sender, const char *path, const char *iface,
		const char *prop, GError **err, gpointer data) {
	return g_variant_new_int32(3);
}

static void acquired(GDBusConnection *c, const char *name, gpointer data) {}

int main(void) {
	GDBusNodeInfo *info = g_dbus_node_info_new_for_xml(xml, NULL);
	GDBusConnection *c = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
	if (!c) return 1;
	static const GDBusInterfaceVTable vt = {call, get_prop, NULL, {0}};
	g_dbus_connection_register_object(c, "/net/reactivated/Fprint/Manager", info->interfaces[0], &vt, NULL, NULL, NULL);
	g_dbus_connection_register_object(c, DEV, info->interfaces[1], &vt, NULL, NULL, NULL);
	g_bus_own_name_on_connection(c, "net.reactivated.Fprint", G_BUS_NAME_OWNER_FLAGS_NONE, acquired, NULL, NULL, NULL);
	g_main_loop_run(g_main_loop_new(NULL, FALSE));
	return 0;
}
