# fetch-commit.sh (sourced): fetch_commit <dir> <commit> <repo url> [tag]
# Puts exactly <commit> into <dir> (a shallow git checkout), whatever a tag name or a server does meanwhile: fetches the
# commit itself by SHA; for FFmpeg it tries the GitHub mirror first (git.ffmpeg.org drops connections now and then), the
# pinned repo next, and the tag name only as a last way to reach the same commit. Three rounds, 15 s apart. Fails (exit
# status 1, with the reason) unless HEAD is <commit> afterwards: a moved tag or an unreachable server never builds
# something else.
fetch_commit() {
    _fc_dir=$1; _fc_commit=$2; _fc_repo=$3; _fc_tag=${4:-}
    _fc_urls=$_fc_repo
    case $_fc_repo in *ffmpeg.org/ffmpeg*|*FFmpeg/FFmpeg*) _fc_urls="https://github.com/FFmpeg/FFmpeg.git $_fc_repo" ;; esac
    [ -d "$_fc_dir/.git" ] || git init -q "$_fc_dir" || return 1
    if [ "$(git -C "$_fc_dir" rev-parse -q --verify "$_fc_commit^{commit}" 2>/dev/null)" != "$_fc_commit" ]; then
        _fc_ok=0
        for _fc_round in 1 2 3; do
            for _fc_url in $_fc_urls; do
                if git -C "$_fc_dir" fetch -q --depth 1 "$_fc_url" "$_fc_commit" 2>/dev/null; then _fc_ok=1; break; fi
                if [ -n "$_fc_tag" ] && git -C "$_fc_dir" fetch -q --depth 1 "$_fc_url" "refs/tags/$_fc_tag:refs/tags/$_fc_tag" 2>/dev/null \
                    && [ "$(git -C "$_fc_dir" rev-parse -q --verify "$_fc_tag^{commit}")" = "$_fc_commit" ]; then _fc_ok=1; break; fi
                echo "fetch_commit: $_fc_commit${_fc_tag:+ ($_fc_tag)} not fetched from $_fc_url (round $_fc_round)" >&2
            done
            [ "$_fc_ok" = 1 ] && break
            [ "$_fc_round" = 3 ] || sleep 15
        done
        [ "$_fc_ok" = 1 ] || { echo "::error::$_fc_commit${_fc_tag:+ ($_fc_tag)}: could not be fetched from $_fc_urls" >&2; return 1; }
    fi
    git -C "$_fc_dir" checkout -q -f "$_fc_commit" || return 1
    [ "$(git -C "$_fc_dir" rev-parse HEAD)" = "$_fc_commit" ] || { echo "::error::$_fc_dir: HEAD is not $_fc_commit" >&2; return 1; }
}
