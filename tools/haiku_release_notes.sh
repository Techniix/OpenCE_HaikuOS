#!/bin/sh
# Writes the notes of a Haiku release (Markdown) to standard output.
#
#   tools/haiku_release_notes.sh <number> <previous commit or ""> <commit> <OpenCE ref> [SHA256SUMS]
#
# <commit> is the commit built; <previous commit> the one the release before
# it was built from (empty for the first release); <OpenCE ref> OpenCE's
# main as fetched (upstream/main), used only to tell OpenCE's commits from
# this fork's. Used by .github/workflows/haiku-release.yml; it can also be
# run by hand to write the notes of an earlier release.
#
# The changes are listed in two parts:
# - From OpenCE: each pull request merged into OpenCE's main, by its title,
#   linked, with its commits under it; then commits made on OpenCE's main
#   directly.
# - Haiku port: this fork's own commits.

set -eu

number=$1
previous=$2
commit=$3
opence_ref=$4
sums=${5:-}

opence_url=https://github.com/OpenCommunityEdition/OpenCE
# (the latest OpenCE commit in this build)
opence=$(git merge-base "$commit" "$opence_ref")

echo "OpenCE for Haiku, build $number: OpenCE's latest changes, merged into the Haiku port and built for Haiku x86_64 (from \`$(git rev-parse --short "$commit")\`, OpenCE \`$(git rev-parse --short "$opence")\`)."
echo

if [ -n "$previous" ] && git cat-file -e "$previous^{commit}" 2>/dev/null; then
	echo "## From OpenCE"
	echo
	listed=0
	# OpenCE's main, newest first, one commit per change it took
	# (a pull request's merge, or a commit made on main directly)
	for change in $(git rev-list --first-parent "$opence" "^$previous"); do
		listed=1
		subject=$(git log -1 --format=%s "$change")
		request=$(printf '%s\n' "$subject" | sed -n 's/^Merge pull request #\([0-9][0-9]*\) from .*/\1/p')
		if [ -n "$request" ]; then
			title=$(git log -1 --format=%b "$change" | sed -n '/./{p;q;}')
			echo "- ${title:-Pull request #$request} ([#$request]($opence_url/pull/$request))"
			count=$(git rev-list --no-merges --count "$change^2" "^$change^1" "^$previous")
			git log --no-merges --format='  - %s' "$change^2" "^$change^1" "^$previous" | head -n 25
			[ "$count" -le 25 ] || echo "  - and $((count - 25)) more ([the pull request]($opence_url/pull/$request/commits))"
		elif [ "$(git rev-list --no-walk --count --merges "$change")" = 0 ]; then
			echo "- $subject"
		fi
	done
	[ "$listed" = 1 ] || echo "Nothing new from OpenCE."
	echo

	echo "## Haiku port"
	echo
	own=$(git log --no-merges --format='- %s' "$commit" "^$previous" "^$opence" | head -n 40)
	echo "${own:-No changes to the Haiku port.}"
	echo
fi

echo "## Checked"
echo
echo "Built in a Haiku R1/beta6 virtual machine, where \`halo --version\` loads the game and names it build-$number. Playing it is not checked automatically: report problems in [Issues](https://github.com/Techniix/OpenCE_HaikuOS/issues)."
echo
echo "## Installing"
echo
echo "Builds 4 and later offer this update themselves when the game starts. Otherwise unzip \`halo-haiku-release.zip\` and start \`halo\` in its folder; it asks for your own Halo: Combat Evolved Xbox disc image the first time (README-Haiku.md)."

if [ -n "$sums" ]; then
	echo
	echo "SHA-256:"
	echo
	echo '```'
	cat "$sums"
	echo '```'
fi
