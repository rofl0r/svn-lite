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

