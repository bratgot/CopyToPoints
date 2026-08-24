#!/usr/bin/env bash
# Update the PUBLIC repository from this private one.
#
# The public repo starts from a fresh history on purpose: this repository's own
# history still contains working notes, the test tree and render output, and
# untracking a file does not remove it from history - anyone with read access
# can still `git show <old-sha>:CLAUDE.md`. So the public repo never sees this
# history at all; it gets the tracked TREE and its own commits.
#
#   tools/publish.sh "what changed in this release"
#
# It refuses to push if the audit below finds anything that should not leave
# this machine. That check is the point of the script - doing this by hand is
# how something gets out.
set -euo pipefail

PUBLIC_REMOTE="${PUBLIC_REMOTE:-https://github.com/bratgot/CopyToPoints.git}"
msg="${1:-}"
if [ -z "$msg" ]; then
  echo "usage: tools/publish.sh \"commit message\""
  exit 1
fi

here="$(cd "$(dirname "$0")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

# the tracked tree at HEAD, and nothing else - no .git, no ignored files
git -C "$here" archive HEAD | tar -x -C "$work"

# ---- the audit -------------------------------------------------------------
fail=0
check() {  # check <description> <count> ; non-zero count fails
  if [ "$2" -ne 0 ]; then
    echo "  REFUSED: $1 ($2)"
    fail=1
  else
    echo "  ok: $1"
  fi
}
echo "auditing what would become public:"
check "no CLAUDE.md"            "$(find "$work" -iname 'CLAUDE.md' | wc -l)"
check "no test tree"            "$(find "$work" -type d \( -name test -o -name NukeTests \) | wc -l)"
check "no logs or caches"       "$(find "$work" -type f \( -name '*.log' -o -name '*.pyc' -o -name '*.autosave' \) | wc -l)"
# --exclude: this script's own patterns are literals, so it matches itself
check "no personal paths"       "$(grep -rl --exclude=publish.sh 'C:/Users/' "$work" 2>/dev/null | wc -l)"
check "no credentials"          "$(grep -rlE --exclude=publish.sh 'ghp_|api[_-]?key|password|BEGIN [A-Z ]*PRIVATE KEY' "$work" 2>/dev/null | wc -l)"
check "LICENSE present"         "$([ -f "$work/LICENSE" ] && echo 0 || echo 1)"
check "third-party notices"     "$([ -f "$work/THIRD_PARTY_NOTICES.md" ] && echo 0 || echo 1)"
[ "$fail" -ne 0 ] && { echo "nothing pushed."; exit 1; }

# ---- publish ---------------------------------------------------------------
# Ask the remote what it actually has before cloning. A bare repo whose HEAD
# names a branch that does not exist clones onto an UNBORN branch: HEAD looks
# unset, the commit below becomes a second root commit, and the push is
# rejected as a non-fast-forward. ls-remote answers the question directly.
pub="$work/.public"
if ! heads="$(git ls-remote --heads "$PUBLIC_REMOTE" 2>/dev/null)"; then
  echo "  cannot reach $PUBLIC_REMOTE"
  exit 1
fi
branch="$(printf '%s\n' "$heads" | sed -n 's#.*refs/heads/##p' | head -1)"

if [ -n "$branch" ]; then
  git clone -q --branch "$branch" "$PUBLIC_REMOTE" "$pub"
  # replace the contents wholesale, so a file deleted here is deleted there
  find "$pub" -mindepth 1 -maxdepth 1 -not -name .git -exec rm -rf {} +
else
  echo "  (public repo has no commits yet - starting its history)"
  branch=main
  mkdir -p "$pub"
  git -C "$pub" init -q -b "$branch"
  git -C "$pub" remote add origin "$PUBLIC_REMOTE"
fi
(cd "$work" && tar -c --exclude=.public .) | tar -x -C "$pub"

git -C "$pub" add -A
if git -C "$pub" diff --cached --quiet; then
  echo "no change to publish."
  exit 0
fi
git -C "$pub" commit -q -m "$msg"
git -C "$pub" push -q origin "$branch"
echo "published $branch to $PUBLIC_REMOTE"
