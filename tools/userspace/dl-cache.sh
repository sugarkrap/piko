piko_seed_dl_cache() {
    _repo="$1"
    _dl="$2"
    [ -d "$_repo/vendor" ] || return 0
    mkdir -p "$_dl"
    for _v in "$_repo"/vendor/*; do
        [ -f "$_v" ] || continue
        _n="${_v##*/}"
        [ -e "$_dl/$_n" ] || ln -s "$_v" "$_dl/$_n"
    done
    unset _repo _dl _v _n
}
