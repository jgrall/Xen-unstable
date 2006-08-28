#
# Copyright (c) 2005 XenSource Ltd.
#
# This library is free software; you can redistribute it and/or
# modify it under the terms of version 2.1 of the GNU Lesser General Public
# License as published by the Free Software Foundation.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# Lesser General Public License for more details.
#
# You should have received a copy of the GNU Lesser General Public
# License along with this library; if not, write to the Free Software
# Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
#


dir=$(dirname "$0")
. "$dir/logging.sh"
. "$dir/xen-script-common.sh"
. "$dir/locking.sh"

exec 2>>/var/log/xen/xen-hotplug.log

export PATH="/sbin:/bin:/usr/bin:/usr/sbin:$PATH"
export LANG="POSIX"
unset $(set | grep ^LC_ | cut -d= -f1)

fatal() {
  xenstore_write "$XENBUS_PATH"/hotplug-status error
  log err "$@"
  exit 1
}

success() {
  # Tell DevController that backend is "connected"
  xenstore_write "$XENBUS_PATH"/hotplug-status connected
}

do_or_die() {
  "$@" || fatal "$@ failed"
}

do_without_error() {
  "$@" 2>/dev/null || log debug "$@ failed"
}

sigerr() {
  fatal "$0 failed; error detected."
}

trap sigerr ERR


##
# xenstore_read <path>+
#
# Read each of the given paths, returning each result on a separate line, or
# exit this script if any of the paths is missing.
#
xenstore_read() {
  local v=$(xenstore-read "$@" || true)
  [ "$v" != "" ] || fatal "xenstore-read $@ failed."
  echo "$v"
}


##
# xenstore_read_default <path> <default>
#
# Read the given path, returning the value there or the given default if the
# path is not present.
#
xenstore_read_default() {
  xenstore-read "$1" 2>/dev/null || echo "$2"
}


##
# xenstore_write (<path> <value>)+
#
# Write each of the key/value pairs to the store, and exit this script if any
# such writing fails.
#
xenstore_write() {
  log debug "Writing $@ to xenstore."
  xenstore-write "$@" || fatal "Writing $@ to xenstore failed."
}


log debug "$@" "XENBUS_PATH=$XENBUS_PATH"
