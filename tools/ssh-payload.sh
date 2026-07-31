# Shared list of the SSH file-transfer binaries tools/build-ssh.sh stages,
# and where each one goes on the device. Sourced (not executed) by every
# path that ships them:
#
#   tools/chunked-deploy.sh      live SSH deploy to a running device
#   flash/build-mtd3-jffs2.sh    the mtd3 "home" ROM image (SD-card flash)
#   flash/build-update-package.sh the offline update.tar
#
# Same reason tools/kernel-modules.sh exists: three copies of a file list
# is three places for it to go stale, and the failure mode here is silent
# (a ROM that boots fine and simply cannot receive files).
#
# Each line is "<path under stage-ssh> <path on device> <mode>".
#
# dropbear itself is deliberately NOT in this list. Every consumer treats
# the list as "always ship"; the SSH SERVER is the one binary that must
# not be replaced without a deliberate opt-in, because this board has no
# serial console and no USB (AGENTS.md) -- a dropbear that fails to start
# is only recoverable with an SD-card recovery flash. Each consumer offers
# its own opt-in (--replace-dropbear / PIKO_SSH_REPLACE_DROPBEAR=1) and
# handles the swap itself, since "how do you safely replace a running
# binary" differs per path (rename-aside over SSH vs. a plain copy into an
# image being built offline).
#
# The sftp-server path is not a preference: /usr/libexec/sftp-server is
# compiled into dropbear (SFTPSERVER_PATH), so a copy anywhere else is
# invisible to the server. Changing it means rebuilding dropbear too.

SSH_PAYLOAD_FILES="usr/bin/scp:usr/bin/scp:755
usr/libexec/sftp-server:usr/libexec/sftp-server:755
usr/bin/dbclient:usr/bin/dbclient:755
usr/bin/dropbearkey:usr/bin/dropbearkey:755"

# The opt-in extra, kept here so its path/mode live with the others.
SSH_PAYLOAD_SERVER="usr/sbin/dropbear:usr/sbin/dropbear:755"
