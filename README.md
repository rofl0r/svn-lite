# svn-lite

A lightweight svn client based on https://github.com/johnmehr/svnup ,
ported to work on linux and enhanced with a svn-compatible command
line parser.

Only dependency is libressl/openssl.

Currently, only the checkout/co action is implemented.

## purpose

Nowadays, almost nobody uses the SVN VCS, but from time to time one
needs to checkout a repo still hosted on SVN.

The official SVN client is a clusterfuck of dependencies, and pulls in
the entire apache library stack, sqlite, python, scons, swig, serf...
a lot of otherwise useless stuff taking up precious hard disk space,
only to download a couple of files.

# build

run `make`.

# svnup mode

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

## future directions

It should be straightforward to add support for a couple
of commands like `svn diff`.
A possible goal is to make the program compatible to
`svn2git` to convert svn repos to git, pretty much the
only other usecase of svn apart from checking out a repo.
