# Contributing

Contributions are welcome.

KVDB is currently an experimental C++ storage engine. The codebase is still changing, so large changes should be discussed before implementation.

## Ways to Contribute

You can help by:

* Fixing bugs
* Improving documentation
* Adding tests
* Improving benchmarks
* Implementing new MemTable data structures
* Reviewing file-format or crash-safety logic
* Improving CMake/build support

## Development Setup

Required tools:

* C++20 compatible compiler
* CMake
* GoogleTest

### Build

```bash
cmake -S . -B build
cmake --build build
```

### Run Tests

```bash
ctest --test-dir build
```

## Code Style

General guidelines:

* Prefer simple and explicit code.
* Keep ownership clear.
* Avoid unnecessary abstractions.
* Return `Status` / `Result<T>` for recoverable errors.
* Use RAII for resources.
* Keep disk-format logic carefully documented.
* Add tests for every non-trivial change.

## Pull Request Guidelines

Before opening a pull request:

* Make sure the project builds.
* Run the test suite.
* Add or update tests if behavior changed.
* Update documentation if the change affects architecture or public behavior.
* Keep pull requests focused on one topic.

Good pull request examples:

* Add AVL MemTable implementation.
* Fix WAL recovery bug.
* Add SSTable corruption test.
* Improve Manifest documentation.

Bad pull request examples:

* Rewrite the whole engine without discussion.
* Mix formatting, refactoring, and feature changes in one PR.
* Change file formats without documentation.

## Reporting Bugs

When reporting a bug, include:

* What happened
* What you expected
* Steps to reproduce
* Operating system/compiler
* Relevant logs or test output
