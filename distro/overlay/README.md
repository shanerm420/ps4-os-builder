# distro/overlay/

Files here are copied over your rootfs before it is packed, so anything you put
here wins. It exists for the changes a console needs that have nothing to do with
your distribution:

- a `ps4` user with a known password, and `sudo`;
- autologin for the display manager;
- a static or DHCP network config;
- the PS4-patched Mesa: `/opt/ps4-mesa`, `/etc/ld.so.conf.d/00-ps4-mesa.conf`,
  `/usr/share/glvnd/egl_vendor.d/10-ps4-mesa.json` and the two
  `libGLX_ps4-mesa.so.0` / `libEGL_ps4-mesa.so.0` symlinks (copy them out of a
  PS4 XFCE Linux image).

This README is the only file the build ignores (`--exclude=./README.md`), so the
directory can exist in git without adding anything to your image.
