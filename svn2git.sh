#!/bin/sh

if test -z "$SVN" ; then
if type svn >/dev/null 2>&1 ; then
SVN=svn
else
SVN="$(dirname $(readlink -f "$0"))"/svn
fi
else
# in case SVN is set to a relative path, resolve it, for chdir reasons.
rp="$(realpath "$SVN" 2>/dev/null)"
test $? = 0 && SVN="$rp"
fi

usage() {
	echo "$0 COMMAND [OPTIONS...]"
	echo "COMMANDs:"
	echo
	echo "convert URL DIR"
	echo "converts the svn repo in URL to a git repo in DIR"
	echo "set env var endrev to a revision number if you dont"
	echo "want to mirror till the last commit"
	echo
	echo "update DIR"
	echo "updates svn repo clone in DIR to latest commit"
	echo "endrev end var can be used too"
	echo
	echo "example:"
	echo "$0 convert svn://svn.code.sf.net/p/dosbox/code-0/dosbox/trunk dosbox"
	echo
	echo "if you want to use a specific SVN program, set SVN env var to it."
	exit 1
}

if test "$1" = convert ; then
test -z "$3" && usage

repo="$2"
outdir="$3"
rev=1
need_init=true

elif test "$1" = update ; then
test -z "$2" && usage
outdir=$2
rev_file="$outdir"/.svnup/revision
test -e "$rev_file" || { echo "can't find $rev_file">&2 ; exit 1; }
rev=$(grep '^rev=' < "$rev_file" | cut -d = -f 2)
rev=$((rev + 1))
repo="$(grep '^url=' < "$rev_file" | cut -d = -f 2)"
need_init=false
else

usage
fi

tmp1=/tmp/svnup2git.1.$$
tmp2=/tmp/svnup2git.2.$$
trap "rm -f $tmp1 $tmp2 2>/dev/null" EXIT TERM INT


test -z "$endrev" && {
	$SVN info "$repo" > $tmp1
	ret=$?
	test $ret = 0 || { echo "error: couldn't fetch current rev from $repo">&2 ; exit 1; }
	endrev=$(grep '^Revision:' < $tmp1 | cut -d ' ' -f 2)
}

if $need_init ; then
	test -d "$outdir" && { "error: $outdir already existing! did you mean to use 'update'?" ; exit 1; }
	mkdir -p "$outdir"
fi
cd "$outdir"

if $need_init ; then
git init
gbranch=master
test "$(basename "$repo" | sed 's,/,,g')" = trunk && gbranch=trunk
git branch -m $gbranch
fi

while test $rev -lt $((endrev + 1)) ; do

$SVN co -r $rev "$repo" . 2> $tmp1
ret=$?
if test $ret != 0 ; then
	path_issue=false
	grep E195012 < $tmp1 >/dev/null && path_issue=true
	grep 'svn: 160013' < $tmp1 >/dev/null && path_issue=true
	grep 'Target path.*does not exist' < $tmp1 >/dev/null && path_issue=true
	if $path_issue ; then
		echo "skipping rev $rev as path does not exit (yet?)"
		rev=$((rev + 1))
		continue
	else
		cat $tmp1
		exit 1
	fi
fi

git add --all .  # . adds *all* files, even dotfiles and dirs not listed with *, same as '*'

# unstage our own files from repo
# we can't check in .gitignore, since that might at some point be added in a commit
for x in $(git status --porcelain | awk '/ \.svn\// || / \.svnup\// {for(i=1;i<=NF;++i) if($i ~ /\.svn\// || $i ~/\.svnup\//) {print($i); break;}}') ; do
git rm --cached "$x" >/dev/null
ret=$?
if test $ret != 0 ; then
  echo "got error trying to git rm $x"
  exit 1
fi
done

$SVN log -r "$rev" . > "$tmp1"
loglines=$(wc -l $tmp1 | cut -d " " -f 1)
test $loglines = 1 && {
	echo "warning: empty revision $rev">&2
	rev=$((rev + 1))
	continue
}
tail -n $((loglines - 1)) < $tmp1 > $tmp2
head -n $((loglines - 2)) < $tmp2 > $tmp1
author=$(head -n 1 < $tmp1 | cut -d '|' -f 2 | sed -e 's/^ //' -e 's/ $//')
author_sane=$(head -n 1 < $tmp1 | cut -d '|' -f 2 | sed -e 's/^ //' -e 's/ $//' -e 's/ /./g')
date=$(head -n 1 < $tmp1 | cut -d '|' -f 3 | sed -e 's/^ //' -e 's/ $//')
printf "r%u|" $rev > $tmp2
tail -n $((loglines - 4)) < $tmp1 >> $tmp2

git commit --author="$author <$author_sane@localhost>" --date="$date" -F $tmp2 --allow-empty

rev=$((rev + 1))
done
