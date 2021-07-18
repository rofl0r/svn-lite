# svn-lite

A lightweight, read-only svn client based on
https://github.com/johnmehr/svnup ,
ported to work on linux and enhanced with a svn-compatible command
line parser.

Only dependency is libressl/openssl.

Currently, the following actions are implemented:

- checkout (equiv to git clone)
- log      (shows commit author, data, message)
- info     (shows current revision)

Additionally, a git2svn tool is shipped that uses svn-lite client
to convert a svn repo into a git repo (and can update it later on).

## purpose

Nowadays, almost nobody uses the SVN VCS, but from time to time one
needs to checkout a repo still hosted on SVN.

The official SVN client is a clusterfuck of dependencies, and pulls in
the entire apache library stack, sqlite, python, scons, swig, serf...
a lot of otherwise useless stuff taking up precious hard disk space,
only to download a couple of files.

# build

run `make`.

# (original) svnup mode

there's a second use mode apart from the traditional interface modeled
after apache's svn command line client, called `svnup`, which basically
just clones a repo according to instructions in a config file and later
on updates the repo whenever executed.

if you want to use the program as `svnup`, just create a symlink from
`svn` to `svnup`:

	ln -s svn svnup

and run ./svnup.

in svnup, a config file is required that looks roughly like this:

```
[defaults]
work_directory=/var/tmp/svnup
host=server.com
protocol=svn

[stuff]
branch=reponame/trunk
target=/path/to/destination
```

using this config file, running svnup like so:

    svnup stuff

is equivalent to

    cd /path/to
    svn co svn://server.com/reponame/trunk destination

this mode might be handy if you only want to update your
local checkout of a single SVN repo from time to time.

