# CGF Projector

Church projection software for **CGF Vernon** — a fork of
[SoftProjector](https://github.com/SoftProjector/softprojector), customized
for our congregation: simplified operator experience, central library sync,
and CGF-specific workflows.

Status: **early transformation stage.** The upstream code is unchanged so far;
guard rails and project direction are being set up first.

## Origin and license

This project is a fork of SoftProjector, © its original authors, licensed
under the **GNU GPL v3** (see [LICENSE](LICENSE)). This fork keeps the same
license: the source code for every distributed build of CGF Projector is
available in this repository. CGF Projector is modified from the original —
please do not report problems with this fork to the SoftProjector authors.

The CGF name, logo, and branding are **not** covered by the GPL and are not
included in this repository. Song lyrics, Bible modules, and all church data
live outside this codebase entirely — the application ships empty.

## Contributing

Read [NEVER-COMMIT.md](NEVER-COMMIT.md) before your first push. This is a
public repository and git history is permanent.

## Building (from upstream)

> To compile the code, build your project in IDE of your choice, copy the EXE
> it produces to a folder. Run the following command to compile the EXE into
> a runnable project (make sure you download the windeployqt tool):

```
.\windeployqt.exe --qmldir "<ProjectDirectory>" "<PathtoEXE>"
```
