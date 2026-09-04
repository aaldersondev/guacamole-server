#!/usr/bin/env bash
#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#
# Installs the guacd image built from this fork.
#
#   install.sh              Point an existing Guacamole stack at this guacd
#   install.sh --new [DIR]  Set up a complete Guacamole stack from scratch
#   install.sh --revert     Put the previous guacd image back
#   install.sh --check      Say what would happen, change nothing
#

set -euo pipefail

SCRIPT_URL="https://raw.githubusercontent.com/aaldersondev/guacamole-server/faster-display/install.sh"
IMAGE_REPO="ghcr.io/aaldersondev/guacd"
IMAGE_TAG="1.6.0-fast"
UPSTREAM_IMAGE="guacamole/guacd:1.6.0"
GUACAMOLE_IMAGE="guacamole/guacamole:1.6.0"
POSTGRES_IMAGE="postgres:16-alpine"

MODE="upgrade"
COMPOSE_FILE=""
TARGET_DIR=""
HTTP_PORT=""
IMAGE_OVERRIDE=""
CHECK=0
ASSUME_YES=0

# Colours, but only when writing to a terminal
if [ -t 1 ]; then
    B=$'\033[1m'; DIM=$'\033[2m'; RED=$'\033[31m'; GREEN=$'\033[32m'
    YELLOW=$'\033[33m'; R=$'\033[0m'
else
    B=""; DIM=""; RED=""; GREEN=""; YELLOW=""; R=""
fi

say()  { printf '%s\n' "$*"; }
step() { printf '%s==>%s %s\n' "$B" "$R" "$*"; }
info() { printf '    %s%s%s\n' "$DIM" "$*" "$R"; }
warn() { printf '%s !%s  %s\n' "$YELLOW" "$R" "$*" >&2; }
ok()   { printf '%s ok%s %s\n' "$GREEN" "$R" "$*"; }
die()  { printf '%serror%s %s\n' "$RED" "$R" "$*" >&2; exit 1; }

usage() {
    cat <<EOF
Installs guacd built from the faster-display fork of guacamole-server.

Usage:
  install.sh [options]              Point an existing Guacamole stack at this guacd
  install.sh --new [DIRECTORY]      Set up a complete Guacamole stack from scratch
  install.sh --revert [options]     Put the previous guacd image back

Options:
  --file PATH     docker-compose.yml to modify (default: autodetected)
  --tag TAG       image tag to use (default: $IMAGE_TAG)
  --image REF     use this image reference outright, instead of $IMAGE_REPO:TAG
  --port PORT     with --new, the port to serve Guacamole on (default: 8080)
  --check         report what would be done, without changing anything
  -y, --yes       do not ask for confirmation
  -h, --help      this message
EOF
}

while [ $# -gt 0 ]; do
    case "$1" in
        --new)     MODE="new"
                   if [ $# -gt 1 ] && [ "${2#-}" = "$2" ]; then TARGET_DIR="$2"; shift; fi ;;
        --revert)  MODE="revert" ;;
        --file)    [ $# -ge 2 ] || die "--file needs a path"; COMPOSE_FILE="$2"; shift ;;
        --tag)     [ $# -ge 2 ] || die "--tag needs a value"; IMAGE_TAG="$2"; shift ;;
        --image)   [ $# -ge 2 ] || die "--image needs a value"; IMAGE_OVERRIDE="$2"; shift ;;
        --port)    [ $# -ge 2 ] || die "--port needs a value"; HTTP_PORT="$2"; shift ;;
        --check)   CHECK=1 ;;
        -y|--yes)  ASSUME_YES=1 ;;
        -h|--help) usage; exit 0 ;;
        *)         usage >&2; die "unknown option: $1" ;;
    esac
    shift
done

IMAGE="${IMAGE_OVERRIDE:-$IMAGE_REPO:$IMAGE_TAG}"

# How to suggest re-running this script. Piped through a shell, "$0" is just
# the name of that shell, which would be useless advice.
if [ -f "$0" ] && [ "${0##*/}" != "bash" ] && [ "${0##*/}" != "sh" ]; then
    SELF="$0"
else
    SELF="curl -fsSL $SCRIPT_URL | bash -s --"
fi

# ---------------------------------------------------------------- environment

DOCKER=""
COMPOSE=""

check_environment() {

    command -v docker >/dev/null 2>&1 \
        || die "Docker is not installed. See https://docs.docker.com/engine/install/"

    if docker info >/dev/null 2>&1; then
        DOCKER="docker"
    elif command -v sudo >/dev/null 2>&1 && sudo -n docker info >/dev/null 2>&1; then
        DOCKER="sudo docker"
        info "using sudo for docker"
    else
        die "Cannot talk to the Docker daemon. Run this as root, or add yourself to the docker group."
    fi

    if $DOCKER compose version >/dev/null 2>&1; then
        COMPOSE="$DOCKER compose"
    elif command -v docker-compose >/dev/null 2>&1; then
        COMPOSE="docker-compose"
    else
        die "Docker Compose is not available. See https://docs.docker.com/compose/install/"
    fi

}

confirm() {

    local reply

    [ "$ASSUME_YES" = 1 ] && return 0

    printf '\n    %s [y/N] ' "$1"

    # Read from the terminal rather than from stdin: this script is meant to be
    # usable as "curl ... | bash", where stdin is the script itself.
    # The timeout keeps an unattended run from waiting forever on a terminal
    # that nobody is sitting at
    if [ -t 0 ]; then
        read -r -t 120 reply || reply=""
    elif [ -r /dev/tty ]; then
        read -r -t 120 reply < /dev/tty || reply=""
    else
        printf '\n'
        die "No terminal to ask on. Re-run with -y to proceed without asking."
    fi

    case "$reply" in [yYoO]*) return 0 ;; *) say "    Cancelled."; exit 0 ;; esac

}

# ------------------------------------------------------------ compose editing

# Prints the line number of the single "image:" line whose value names a guacd
# image, or fails if there is not exactly one.
guacd_image_line() {
    local file="$1" matches
    matches=$(grep -nE '^[[:space:]]*image:[[:space:]]*[^[:space:]#]*guacd' "$file" || true)
    [ -n "$matches" ] || die "No guacd image found in $file. Is this a Guacamole stack?"
    [ "$(printf '%s\n' "$matches" | wc -l)" -eq 1 ] \
        || die "Several guacd images found in $file. Edit it by hand, or use --file."
    printf '%s\n' "${matches%%:*}"
}

# Prints the current value of the guacd image line.
guacd_image_value() {
    sed -n "$2s/^[[:space:]]*image:[[:space:]]*//p" "$1" | tr -d '\r'
}

# Prints the name of the compose service that the given line belongs to, by
# walking back to the nearest key indented less than the line itself.
guacd_service_name() {
    # Two passes over the file: the first finds how far the image line is
    # indented, the second reports the last bare key indented less than that,
    # which is the service the line belongs to.
    awk -v target="$2" '
        NR == FNR {
            if (FNR == target) { match($0, /^[ \t]*/); target_indent = RLENGTH }
            next
        }
        FNR == target { print name; exit }
        /^[ \t]*[A-Za-z0-9_.-]+:[ \t]*(#.*)?$/ {
            match($0, /^[ \t]*/)
            if (RLENGTH > 0 && RLENGTH < target_indent) {
                key = $0
                sub(/^[ \t]*/, "", key)
                sub(/:.*$/, "", key)
                name = key
            }
        }
    ' "$1" "$1"
}

# Rewrites the given line to use the given image, preserving indentation.
set_image_line() {
    local file="$1" line="$2" image="$3" tmp
    tmp=$(mktemp "${file}.XXXXXX")
    awk -v n="$line" -v img="$image" '
        NR == n {
            match($0, /^[ \t]*/)
            printf "%simage: %s\n", substr($0, 1, RLENGTH), img
            next
        }
        { print }
    ' "$file" > "$tmp"
    cat "$tmp" > "$file"
    rm -f "$tmp"
}

# Locates the compose file of a running Guacamole stack, if there is one.
detect_compose_file() {

    local id path

    # A running guacd container knows which compose file created it
    for id in $($DOCKER ps -q 2>/dev/null); do
        case "$($DOCKER inspect -f '{{.Config.Image}}' "$id" 2>/dev/null)" in
            *guacd*)
                path=$($DOCKER inspect -f \
                    '{{index .Config.Labels "com.docker.compose.project.config_files"}}' \
                    "$id" 2>/dev/null | cut -d, -f1)
                if [ -n "$path" ] && [ -f "$path" ]; then
                    printf '%s\n' "$path"
                    return 0
                fi
                ;;
        esac
    done

    # Otherwise try the usual places
    for path in \
        "$PWD/docker-compose.yml" "$PWD/docker-compose.yaml" \
        "$PWD/compose.yml" "$PWD/compose.yaml" \
        /opt/guacamole/docker-compose.yml \
        /etc/guacamole/docker-compose.yml \
        "$HOME/guacamole/docker-compose.yml"
    do
        if [ -f "$path" ] && grep -qE '^[[:space:]]*image:[[:space:]]*[^[:space:]#]*guacd' "$path"; then
            printf '%s\n' "$path"
            return 0
        fi
    done

    return 1
}

wait_for_guacd() {
    local dir="$1" service="$2" i=0
    while [ "$i" -lt 40 ]; do
        if (cd "$dir" && $COMPOSE logs "$service" 2>/dev/null) | grep -q "Listening on host"; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    return 1
}

# --------------------------------------------------------------------- modes

do_upgrade() {

    local revert="${1:-0}" dir service line current target backup

    check_environment

    if [ -z "$COMPOSE_FILE" ]; then
        COMPOSE_FILE=$(detect_compose_file) \
            || die "No Guacamole stack found. Use --file PATH, or --new to set one up."
    fi
    [ -f "$COMPOSE_FILE" ] || die "No such file: $COMPOSE_FILE"

    dir=$(cd "$(dirname "$COMPOSE_FILE")" && pwd)
    line=$(guacd_image_line "$COMPOSE_FILE")
    current=$(guacd_image_value "$COMPOSE_FILE" "$line")
    service=$(guacd_service_name "$COMPOSE_FILE" "$line")
    [ -n "$service" ] || service="guacd"

    if [ "$revert" = 1 ]; then
        backup=$(ls -1t "$COMPOSE_FILE".bak-guacd-* 2>/dev/null | head -1 || true)
        if [ -n "$backup" ]; then
            target=$(guacd_image_value "$backup" "$(guacd_image_line "$backup")")
            info "previous image taken from $(basename "$backup")"
        else
            target="$UPSTREAM_IMAGE"
            warn "no backup found, falling back to $UPSTREAM_IMAGE"
        fi
    else
        target="$IMAGE"
    fi

    step "Guacamole stack"
    info "compose file : $COMPOSE_FILE"
    info "service      : $service"
    info "current      : $current"
    info "new          : $target"

    if [ "$current" = "$target" ]; then
        ok "Already using $target. Nothing to do."
        return 0
    fi

    if [ "$CHECK" = 1 ]; then
        say ""
        say "    --check: nothing was changed. Without it, this run would:"
        say "      1. pull $target"
        say "      2. back up $(basename "$COMPOSE_FILE")"
        say "      3. set the image of service \"$service\" to $target"
        say "      4. restart that service and wait for it to come up"
        return 0
    fi

    confirm "Switch service \"$service\" to $target?"

    # Make sure the image is there before touching anything, so that a registry
    # problem cannot leave the stack pointing at an image that does not exist
    if $DOCKER image inspect "$target" >/dev/null 2>&1; then
        step "Image $target is already present locally"
    else
        step "Pulling $target"
        $DOCKER pull "$target" >/dev/null 2>&1             || die "Could not pull $target. Check the tag, or build it locally with: docker build -t $target ."
        ok "pulled"
    fi

    backup="$COMPOSE_FILE.bak-guacd-$(date +%Y%m%d%H%M%S)"
    cp -p "$COMPOSE_FILE" "$backup"
    step "Backed up to $(basename "$backup")"

    set_image_line "$COMPOSE_FILE" "$line" "$target"

    step "Restarting $service"
    if ! (cd "$dir" && $COMPOSE up -d "$service" >/dev/null 2>&1) || ! wait_for_guacd "$dir" "$service"; then
        warn "$service did not come up. Putting the previous image back."
        set_image_line "$COMPOSE_FILE" "$line" "$current"
        (cd "$dir" && $COMPOSE up -d "$service" >/dev/null 2>&1) || true
        die "Restart failed. The stack was left as it was; see: $COMPOSE logs $service"
    fi

    ok "$service is running $target"
    say ""
    if [ "$revert" = 1 ]; then
        say "    Back on $target. To switch to the faster guacd again:"
        say "      $SELF --file $COMPOSE_FILE"
    else
        say "    Reconnect and the session should feel lighter. To go back:"
        say "      $SELF --revert --file $COMPOSE_FILE"
    fi
}

do_new() {

    local dir port pass

    check_environment

    dir="${TARGET_DIR:-$PWD/guacamole}"
    port="${HTTP_PORT:-8080}"

    step "New Guacamole stack"
    info "directory : $dir"
    info "guacd     : $IMAGE"
    info "web app   : $GUACAMOLE_IMAGE"
    info "database  : $POSTGRES_IMAGE"
    info "url       : http://localhost:$port/guacamole/"

    if [ -e "$dir/docker-compose.yml" ]; then
        die "$dir/docker-compose.yml already exists. Run without --new to switch it over."
    fi

    if $DOCKER ps --format '{{.Ports}}' 2>/dev/null | grep -q ":$port->"; then
        die "Port $port is already in use. Choose another with --port."
    fi

    if [ "$CHECK" = 1 ]; then
        say ""
        say "    --check: nothing was created. Without it, this run would:"
        say "      1. create $dir"
        say "      2. generate the database schema and a random database password"
        say "      3. write docker-compose.yml and .env"
        say "      4. start the stack and wait for it to answer on port $port"
        return 0
    fi

    confirm "Create this stack?"

    mkdir -p "$dir/data/postgres" "$dir/data/drive" "$dir/data/record"

    step "Generating the database schema"
    $DOCKER run --rm "$GUACAMOLE_IMAGE" /opt/guacamole/bin/initdb.sh --postgresql \
        > "$dir/initdb.sql" 2>/dev/null \
        || die "Could not generate the schema from $GUACAMOLE_IMAGE"
    ok "$(wc -l < "$dir/initdb.sql") lines of SQL"

    # NOTE: read a bounded amount of randomness first. Piping /dev/urandom
    # straight into head closes the pipe under tr, which fails the pipeline.
    pass=$(head -c 256 /dev/urandom | LC_ALL=C tr -dc 'A-Za-z0-9' | cut -c1-32)
    [ "${#pass}" -ge 24 ] || die "Could not generate a database password"
    printf 'POSTGRES_PASSWORD=%s\nHTTP_PORT=%s\n' "$pass" "$port" > "$dir/.env"
    chmod 600 "$dir/.env"

    cat > "$dir/docker-compose.yml" <<EOF
#
# Guacamole, using the guacd image from
# https://github.com/aaldersondev/guacamole-server
#
# Generated by install.sh. The database password lives in .env.
#

services:

  postgres:
    image: $POSTGRES_IMAGE
    restart: unless-stopped
    environment:
      POSTGRES_DB: guacamole_db
      POSTGRES_USER: guacamole_user
      POSTGRES_PASSWORD: \${POSTGRES_PASSWORD}
    volumes:
      - ./data/postgres:/var/lib/postgresql/data
      # Read once, on the very first start, and ignored afterwards
      - ./initdb.sql:/docker-entrypoint-initdb.d/initdb.sql:ro
    healthcheck:
      test: ["CMD-SHELL", "pg_isready -U guacamole_user -d guacamole_db"]
      interval: 10s
      timeout: 5s
      retries: 10

  guacd:
    image: $IMAGE
    restart: unless-stopped
    volumes:
      - ./data/drive:/drive
      - ./data/record:/record

  guacamole:
    image: $GUACAMOLE_IMAGE
    restart: unless-stopped
    depends_on:
      postgres:
        condition: service_healthy
      guacd:
        condition: service_started
    environment:
      GUACD_HOSTNAME: guacd
      POSTGRESQL_HOSTNAME: postgres
      # NOTE: these are compose service names, resolved through the network
      # alias compose sets up. Reattaching containers to a pre-existing network
      # can drop those aliases, and the only symptom is "Unexpected internal
      # error" on every login, with nothing anywhere pointing at DNS. If that
      # ever happens, "docker compose down" then "up -d" - "up -d" alone will
      # not recreate the network.
      POSTGRESQL_PORT: 5432
      POSTGRESQL_DATABASE: guacamole_db
      POSTGRESQL_USER: guacamole_user
      POSTGRESQL_PASSWORD: \${POSTGRES_PASSWORD}
      RECORDING_SEARCH_PATH: /record
    ports:
      - "\${HTTP_PORT}:8080"
EOF

    step "Starting the stack"
    (cd "$dir" && $COMPOSE up -d >/dev/null 2>&1) || die "Could not start the stack. See: cd $dir && $COMPOSE logs"

    local i=0
    while [ "$i" -lt 90 ]; do
        if curl -fsS -o /dev/null "http://localhost:$port/guacamole/" 2>/dev/null; then
            break
        fi
        sleep 2
        i=$((i + 2))
    done

    if [ "$i" -ge 90 ]; then
        warn "Guacamole is not answering on port $port yet. It may still be starting."
        warn "Check with: cd $dir && $COMPOSE logs guacamole"
    else
        ok "Guacamole is answering on port $port"
    fi

    say ""
    say "    ${B}http://localhost:$port/guacamole/${R}"
    say "    login ${B}guacadmin${R} / password ${B}guacadmin${R}"
    say ""
    warn "Change that password before letting anyone near this, and put a reverse"
    warn "proxy with TLS in front of it before exposing it to the internet."
}

case "$MODE" in
    upgrade) do_upgrade 0 ;;
    revert)  do_upgrade 1 ;;
    new)     do_new ;;
esac
