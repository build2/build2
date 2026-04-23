# build2 - build2 toolchain build system

`build2` is an open source (MIT), cross-platform build toolchain that provides
sufficient depth and flexibility to develop and package complex C/C++
projects. The toolchain is a hierarchy of tools consisting of a
general-purpose build system, package manager (for package consumption), and
project manager (for project development). This package contains the `build2`
build system.

For more information refer to the [`build2` home page](https://build2.org) and
the [`build2` project organization](https://github.com/build2/) on GitHub.

This `README` file contains information that is more appropriate for
development or packaging of `build2`. If you simply want to install and use
it, then rather refer to the [installation
instructions](https://build2.org/install.xhtml). Note also that the packaged
[development snapshots](https://build2.org/community.xhtml#stage) are
available as well.

## Packaging

If you wish to package the toolchain (for example, for a Linux distribution),
then the recommended starting point is the [Toolchain Installation and
Upgrade](https://build2.org/build2-toolchain/doc/build2-toolchain-install.xhtml)
documentation.

Note that if you are only interested in packaging the build system, you could
instead use the steps described in the accompanying `INSTALL` file.

## Development

Setting up the environment to develop the `build2` toolchain itself is
somewhat complicated because we use it for its own development and that often
poses chicken-and-egg kinds of problems. For details see the [_How do I setup
the build2 toolchain for
development?_](https://github.com/build2/HOWTO/blob/master/entries/setup-build2-for-development.md)
HOWTO article.

Note also that if you are only interested in the build system, you could
instead bootstrap the development setup using the steps described in the
accompanying `INSTALL` file.
