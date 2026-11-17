# Testing fl-network against a real NetworkManager

NetworkManager needs udev to manage devices, so use a privileged container
built from the Debian build image (packages: network-manager, udev, dbus,
grim, wtype) with the build tree in /build/out:

```sh
docker run -d --init --privileged --name fldnm IMAGE sleep infinity
docker exec fldnm bash -c '
  mkdir -p /run/dbus /etc/NetworkManager/conf.d
  printf "[keyfile]\nunmanaged-devices=interface-name:eth0\n" > /etc/NetworkManager/conf.d/99-test.conf
  rm -f /run/dbus/pid; dbus-daemon --system --fork
  /usr/lib/systemd/systemd-udevd --daemon; udevadm trigger --action=add; udevadm settle
  ip link add veth-a type veth peer name veth-b; ip link add dummy0 type dummy
  NetworkManager --no-daemon > /tmp/nm.log 2>&1 &
  sleep 4; nmcli device set veth-a managed yes; nmcli device set dummy0 managed yes'
```

Then run `fl-wm` headless with `fl-network`, drive it with `fl-vptr`
(tests/tools) and `wtype`, and check the result with `nmcli`. Verified this
way: device details, editing IPv4 (method, address, gateway, DNS), adding a
wired connection, connect, disconnect and delete. Wi-Fi needs real hardware or
the mac80211_hwsim kernel module and was not tested in a container.
