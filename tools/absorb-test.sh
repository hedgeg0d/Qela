#!/bin/sh
set -eu

QELA="${QELA:-build/bootstrap/s2}"
QELA=$(readlink -f "$QELA")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

mkdir -p "$tmp/somelib" "$tmp/project"
cat > "$tmp/somelib/value.qela" <<'EOF'
fn absorbed_value() int { return 42; }
EOF
cat > "$tmp/project/main.qela" <<'EOF'
import "somelib/value.qela";
fn main() int { return absorbed_value(); }
EOF

"$QELA" absorb "$tmp/somelib" -o "$tmp/qela+" >/dev/null
[ "$("$tmp/qela+" absorbed)" = somelib ]

set +e
(cd "$tmp/project" && "$tmp/qela+" main.qela -o app && ./app)
status=$?
set -e
[ "$status" -eq 42 ]

"$tmp/qela+" absorbed drop somelib >/dev/null
[ -z "$("$tmp/qela+" absorbed)" ]
cmp "$QELA" "$tmp/qela+"

"$QELA" absorb "$tmp/somelib/value.qela" -o "$tmp/qela-file" >/dev/null
[ "$("$tmp/qela-file" absorbed)" = value.qela ]
"$tmp/qela-file" absorbed drop >/dev/null
cmp "$QELA" "$tmp/qela-file"

cp "$QELA" "$tmp/qela-self"
"$tmp/qela-self" absorb "$tmp/somelib" >/dev/null
[ "$("$tmp/qela-self" absorbed)" = somelib ]
"$tmp/qela-self" absorbed drop >/dev/null
cmp "$QELA" "$tmp/qela-self"

printf 'ok: absorb, import, list and drop\n'
