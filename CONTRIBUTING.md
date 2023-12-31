# Contributing to Virtio Loopback Device

Thank you for your coming the Unified HMI project whose repository hosted by
Panasonic Automotive Systems Co. Ltd (“PAS”). We'd love to accept your various
kinds of contributions like

- Bug report
- Feature request
- Pull request
- Source code review

and etc.


## Submit Pull Request

Please follow following rules when submitting a PR:

- PR should be created against master branch.
- Follow are expected for commits included in PR.
  - Have a descriptive title.
  - As small as possible to do an isolated and completed function.
  - Use "git clang-format" to assure the [Coding Style](documentation/coding-style.md).

In addition to the rules above, by submitting a pull request, you must
represent that you (i) are the copyright owner or legal entity authorized by
the copyright owner (ii)  have the right to license Your Contribution (iii)
accept and agree to license to PAS and this community under the GNU General Public License, version 2.

In each Your Contribution, You need to submit now or in the future to certify
with the following terms of the Developer Certificate of Origin (“DCO”) Version 1.1.
To represent that You have read and agreed to DCO and the above, You
sign-off Your Contribution by adding a line with your real name (not pseudonyms
or anonymous) and e-mail address to every git commit message:

	“Signed-off-by: Jordan Smith johdan.smith@example.com”


```
Developer Certificate of Origin
Version 1.1
Copyright (C) 2004, 2006 The Linux Foundation and its contributors.

Everyone is permitted to copy and distribute verbatim copies of this license document, but changing it is not allowed.

Developer's Certificate of Origin 1.1
By making a contribution to this project, I certify that:
(a) The contribution was created in whole or in part by me and I have the right to submit it under the open source license indicated in the file; or
(b) The contribution is based upon previous work that, to the best of my knowledge, is covered under an appropriate open source license and I have the right under that license to submit that work with modifications, whether created in whole or in part by me, under the same open source license (unless I am permitted to submit under a different license), as indicated in the file; or
(c) The contribution was provided directly to me by some other person who certified (a), (b) or (c) and I have not modified it.
(d) I understand and agree that this project and the contribution are public and that a record of the contribution (including all personal information I submit with it, including my sign-off) is maintained indefinitely and may be redistributed consistent with this project or the open source license(s) involved.

```

# Virtio Loopback Device Coding Style

[Linux kernel coding style](https://www.kernel.org/doc/html/v5.18/process/coding-style.html) is used for RVGPU development. The basic idea is following:

* Linux kernel coding style is a mature and fully validated coding style.
* RVGPU in fact provides a low-level system function on Linux and also includes
  a Linux kernel module. It has a good fit for Linux kernel coding style.
* Linux kernel provides the coding style definition for clang-format which is
  an excellent tool to format C code.

We have included the .clang-format file from the Linux kernel v5.18 source tree
to the root of RVGPU source. So you can use clang-format tools to format the
source code and the patches to be committed.

## Using clang-format

In this section, a simple usage of clang-format is described. You can install
clang-format as following in Ubuntu:

```
	sudo apt install clang-format
```

which will install following two tools:

* clang-format
* git-clang-format

clang-format can be used to formatting the specified source code file as follow:

```
	clang-format src/rvgpu-renderer/rvgpu-renderer.c
```

Formatted rvgpu-renderer.c will be outputted to the standard output. Note that
because clang-format will use the coding style defined in the .clang-format
file of the current directory, please make sure clang-format is executed from
the root of RVGPU source tree.

If a "-i" option is used, clang-format will modify the source file directly
instead of print out to standard output.

```
	clang-format -i src/rvgpu-renderer/rvgpu-renderer.c
```

When using together with git, you can only format the modified part instead of
the whole source code file by using git-clang-format. This does great help to
create a clean commit.

git-clang-format can be used as follow:

    # Modify source code
    vim src/rvgpu-renderer/rvgpu-renderer.c

    # Stage the modification
    git add src/rvgpu-renderer/rvgpu-renderer.c

    # Format the modification
    git clang-format

If git-clang-format modified the source code,
src/rvgpu-renderer/rvgpu-renderer.c will turn to be "modified" state again.
Just confirm and git-add it again.

It is strongly recommended to use git-clang-format check the modifications you
made before committing them.
