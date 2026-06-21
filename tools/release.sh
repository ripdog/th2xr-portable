#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'USAGE'
Usage: tools/release.sh [--yes] [--dry-run]

Creates and pushes an annotated v$VERSION tag, which triggers
.github/workflows/release.yml to create a draft GitHub release and upload
build artifacts.

This script intentionally pauses so you remember to bump VERSION first.

Options:
  -y, --yes      Skip the interactive confirmation prompt.
  -n, --dry-run  Print the release steps without changing git state.
  -h, --help     Show this help.
USAGE
}

confirm=false
dry_run=false
while (($#)); do
    case "$1" in
        -y|--yes) confirm=true ;;
        -n|--dry-run) dry_run=true ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 2
            ;;
    esac
    shift
done

cd "$(git rev-parse --show-toplevel)"

version="$(tr -d '[:space:]' < VERSION)"
if [[ ! "$version" =~ ^[0-9]+(\.[0-9]+){1,3}(-[0-9A-Za-z][0-9A-Za-z.-]*)?$ ]]; then
    echo "VERSION must look like 0.1.2 or 1.0-test; got '$version'." >&2
    exit 1
fi
tag="v$version"
branch="$(git branch --show-current)"
remote="${RELEASE_REMOTE:-origin}"

echo "VERSION says: $version"
echo "Release tag:  $tag"
if latest_tag="$(git tag --list 'v*' --sort=-v:refname | head -n 1)"; then
    if [[ -n "$latest_tag" ]]; then
        echo "Latest tag:   $latest_tag"
    fi
fi
echo
echo "Reminder: bump VERSION before releasing. Android versionName is derived from it."
echo

if git rev-parse -q --verify "refs/tags/$tag" >/dev/null; then
    echo "Tag $tag already exists locally." >&2
    exit 1
fi
if git ls-remote --exit-code --tags "$remote" "refs/tags/$tag" >/dev/null 2>&1; then
    echo "Tag $tag already exists on $remote." >&2
    exit 1
fi

status="$(git status --porcelain)"
if [[ -n "$status" ]]; then
    echo "Working tree is not clean:"
    git status --short
    echo
    echo "Commit your VERSION bump and release changes before tagging." >&2
    exit 1
fi

if [[ -z "$branch" ]]; then
    echo "You are in detached HEAD; checkout a branch before releasing." >&2
    exit 1
fi

if ! $confirm; then
    echo "About to:"
    echo "  git tag -a $tag -m $tag"
    echo "  git push $remote $branch"
    echo "  git push $remote $tag"
    echo
    read -r -p "Type '$tag' to confirm you bumped VERSION and want to release: " answer
    if [[ "$answer" != "$tag" ]]; then
        echo "Aborted."
        exit 1
    fi
fi

run() {
    if $dry_run; then
        printf '+ %q' "$@"
        printf '\n'
    else
        "$@"
    fi
}

run git tag -a "$tag" -m "$tag"
run git push "$remote" "$branch"
run git push "$remote" "$tag"

echo
echo "Release workflow triggered for $tag."
echo "Watch: https://github.com/ripdog/th2xr-portable/actions/workflows/release.yml"
