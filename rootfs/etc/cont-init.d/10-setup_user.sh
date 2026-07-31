#!/usr/bin/env bash
# steam-stream-safe-setup-user -- DO NOT REMOVE THIS MARKER (asserted at image build time)
#
# Replaces the GOW base image's /etc/cont-init.d/10-setup_user.sh, which does
# `userdel -r "$(id -nu "${PUID}")"` whenever uid ${PUID} is taken. On the first boot of a
# container that account is `ubuntu` (harmless); on every LATER START of the same container it
# is `retro`, whose home is a bind mount holding Steam's login/library and the Moonlight
# identity (.steam-stream/{cert.pem,key.pem,uuid,clients/*.pem}) -- `-r` then recursively
# deletes the contents of that mount. This version adapts the existing account in place and
# never removes a home directory.
#
# The entrypoint `source`s this as PID 1 under `set -uo pipefail`, so: no `set -e` (it would
# leak into the rest of boot), no `exit` (it would kill PID 1), every expansion defaulted.

source /opt/gow/bash-lib/utils.sh

_ss_setup_user() {
  local uname="${UNAME:-retro}"
  local home_dir="${HOME:-/home/retro}"
  local puid="${PUID:-1000}"
  local pgid="${PGID:-1000}"

  if [[ "${uname}" == "root" ]]; then
    gow_log "[10-setup_user] UNAME=root -- nothing to do"
    return 0
  fi

  local gname
  gname="$(getent group "${pgid}" | cut -d: -f1)"
  if [[ -z "${gname}" ]]; then
    if getent group "${uname}" >/dev/null; then
      groupmod -g "${pgid}" "${uname}" || gow_log "[10-setup_user] WARN: groupmod -g failed"
    else
      groupadd -g "${pgid}" "${uname}" || gow_log "[10-setup_user] WARN: groupadd failed"
    fi
  elif [[ "${gname}" != "${uname}" ]] && ! getent group "${uname}" >/dev/null; then
    groupmod -n "${uname}" "${gname}" || gow_log "[10-setup_user] WARN: groupmod -n failed"
  fi

  local uid_owner
  uid_owner="$(getent passwd "${puid}" | cut -d: -f1)"

  if getent passwd "${uname}" >/dev/null; then
    local cur_uid cur_gid cur_home _x
    IFS=: read -r _x _x cur_uid cur_gid _x cur_home _x < <(getent passwd "${uname}")
    if [[ "${cur_uid}" == "${puid}" && "${cur_gid}" == "${pgid}" && "${cur_home}" == "${home_dir}" ]]; then
      # The hot path on every restart after the first: touch nothing at all.
      gow_log "[10-setup_user] ${uname} already uid=${puid} gid=${pgid} home=${home_dir} -- no changes"
    else
      if [[ -n "${uid_owner}" && "${uid_owner}" != "${uname}" ]]; then
        # userdel WITHOUT -r only edits passwd/shadow/group; it never touches the filesystem.
        gow_log "[10-setup_user] uid ${puid} held by '${uid_owner}' -- dropping that passwd entry (home kept)"
        userdel "${uid_owner}" || gow_log "[10-setup_user] WARN: userdel ${uid_owner} failed"
      fi
      gow_log "[10-setup_user] adjusting ${uname}: uid ${cur_uid}->${puid} gid ${cur_gid}->${pgid} home ${cur_home}->${home_dir}"
      usermod -u "${puid}" -g "${pgid}" -d "${home_dir}" -s /bin/bash "${uname}" \
        || gow_log "[10-setup_user] WARN: usermod ${uname} failed"
    fi
  elif [[ -n "${uid_owner}" ]]; then
    # First boot: the image ships `ubuntu` on uid ${puid}. Rename it -- without -m, so nothing
    # is moved or copied and /home/ubuntu is simply left where it is.
    gow_log "[10-setup_user] renaming image user '${uid_owner}' (uid ${puid}) -> '${uname}', home ${home_dir}"
    usermod -l "${uname}" -g "${pgid}" -d "${home_dir}" -s /bin/bash "${uid_owner}" \
      || gow_log "[10-setup_user] WARN: usermod -l ${uid_owner} -> ${uname} failed"
  else
    # -M, never -m: the home is a mount point we create and chown ourselves.
    gow_log "[10-setup_user] creating ${uname} (uid ${puid} gid ${pgid} home ${home_dir})"
    useradd -M -d "${home_dir}" -u "${puid}" -g "${pgid}" -s /bin/bash "${uname}" \
      || gow_log "[10-setup_user] WARN: useradd ${uname} failed"
  fi

  mkdir -p "${home_dir}"
  chown "${puid}:${pgid}" "${home_dir}" 2>/dev/null || true
  chmod 755 "${home_dir}" 2>/dev/null || true

  # SteamOS-compat path some GOW scripts and Steam configs expect.
  if [[ "${home_dir}" != "/home/deck" && ! -e /home/deck ]]; then
    ln -s "${home_dir}" /home/deck 2>/dev/null || true
  fi

  if [[ -n "${XDG_RUNTIME_DIR:-}" && -d "${XDG_RUNTIME_DIR}" ]]; then
    chown -R "${puid}:${pgid}" "${XDG_RUNTIME_DIR}" 2>/dev/null || true
  fi

  umask "${UMASK:-000}" 2>/dev/null || true
  gow_log "[10-setup_user] passwd: $(getent passwd "${uname}")"
  return 0
}

_ss_setup_user
unset -f _ss_setup_user
