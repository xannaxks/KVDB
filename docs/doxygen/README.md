# KVDB API documentation

KVDB uses [Doxygen](https://www.doxygen.nl/) to generate API documentation from
the source files in `include/` and `src/`. The repository already contains the
project configuration in `docs/doxygen/Doxyfile`.

## Prerequisites

Install the following tools and make sure their executables are available on
`PATH`:

- **Doxygen** is required. The checked-in configuration was generated with
  Doxygen 1.17.0; using that version or a newer compatible version is
  recommended.
- **Graphviz** is required by the current configuration because `HAVE_DOT` is
  set to `YES`. Doxygen uses its `dot` executable for dependency, inheritance,
  and collaboration diagrams.
- **A LaTeX distribution** is optional. Install TeX Live, MiKTeX, or another
  distribution that provides `pdflatex` and `makeindex` only if you want to
  compile the generated LaTeX into a PDF. It is not needed for HTML output.
- **GNU Make** is needed to build the PDF with the generated `Makefile` on
  Linux or macOS. On Windows, Doxygen also generates `make.bat`.

Use the official [Doxygen download page](https://www.doxygen.nl/download.html),
[Doxygen installation guide](https://www.doxygen.nl/manual/install.html), and
[Graphviz download page](https://graphviz.org/download/) for platform-specific
installation instructions.

Verify the required tools before generating the documentation:

```sh
doxygen --version
dot -V
```

To build the optional PDF, also verify:

```sh
pdflatex --version
makeindex --version
```

## Generate the documentation

Run Doxygen **from `docs/doxygen`**. The working directory matters: the
checked-in `Doxyfile` uses `../../include` and `../../src` as input paths and
uses the current directory as its output directory.

From the repository root:

```sh
cd docs/doxygen
doxygen Doxyfile
```

Doxygen generates:

- `docs/doxygen/html/` for HTML documentation. Open `html/index.html` in a web
  browser.
- `docs/doxygen/latex/` for LaTeX sources. These are an intermediate format for
  producing a PDF.

Warnings are written to the terminal. Review them before publishing generated
documentation, especially warnings about undocumented members, unresolved
references, missing input files, or failed Graphviz commands.

## Build the PDF

First generate the documentation as described above. Then, while still in
`docs/doxygen`, run the command for your platform.

Linux or macOS:

```sh
cd latex
make
```

Windows PowerShell:

```powershell
Set-Location latex
.\make.bat
```

The resulting file is `docs/doxygen/latex/refman.pdf`.

## Create or update a Doxyfile

The repository's `Doxyfile` is already customized, so normal documentation
generation does not require creating another one.

To experiment with a fresh configuration template without overwriting the
checked-in file, run:

```sh
cd docs/doxygen
doxygen -g Doxyfile.new
```

Edit the generated file and set at least the project name, input paths, output
formats, and Graphviz options. The official
[configuration reference](https://www.doxygen.nl/manual/config.html) describes
every available setting.

Do not run `doxygen -g Doxyfile` unless replacing the project configuration is
intentional. If `Doxyfile` already exists, Doxygen renames it to
`Doxyfile.bak` before creating the new template.

When upgrading the configuration for a newer Doxygen release, make a backup or
commit the current file first, and then run:

```sh
cd docs/doxygen
doxygen -u Doxyfile
```

The `-u` option carries existing settings into a configuration for the current
Doxygen version and adds new options with their defaults, but custom comments
in the original file are not preserved. Review the diff before committing the
updated configuration.

To display only settings that differ from Doxygen's defaults, use:

```sh
doxygen -x Doxyfile
```

## Doxygen documentation

- [Doxygen manual](https://www.doxygen.nl/manual/)
- [Getting started](https://www.doxygen.nl/manual/starting.html)
- [Command-line usage](https://www.doxygen.nl/manual/doxygen_usage.html)
- [Configuration reference](https://www.doxygen.nl/manual/config.html)
- [Documenting source code](https://www.doxygen.nl/manual/docblocks.html)
- [Special commands](https://www.doxygen.nl/manual/commands.html)
- [Graphs and diagrams](https://www.doxygen.nl/manual/diagrams.html)
- [Output formats](https://www.doxygen.nl/manual/output.html)
