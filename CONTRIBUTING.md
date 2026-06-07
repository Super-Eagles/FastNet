# Contributing to FastNet

Thank you for your interest in contributing to FastNet! To maintain a high-quality codebase, please follow these guidelines when submitting bug reports, suggesting features, or submitting pull requests.

## Code of Conduct

By participating in this project, you agree to maintain a respectful, welcoming, and professional environment for everyone.

## How to Contribute

### 1. Reporting Bugs
- Search existing issues to ensure the bug hasn't already been reported.
- Open a new issue using the **Bug Report** template.
- Provide a minimal, reproducible example if possible, along with your platform (OS, Compiler version, CMake version).

### 2. Suggesting Features
- Open an issue using the **Feature Request** template.
- Clearly describe the problem you want to solve and your proposed solution or API design.

### 3. Submitting Pull Requests
- Fork the repository and create a branch from `develop` (or `main` if `develop` doesn't exist).
- Adhere to the C++17 standards used in the codebase.
- **Code Style**: Run `clang-format` on all modified files using the project's `.clang-format` configuration.
- **Tests**: Ensure all existing tests pass and write new regression tests under `test/` for any new functionality or bug fixes.
- Keep PRs focused on a single issue or feature.

## Development Setup

### Build & Run Tests
Refer to [VALIDATION_CHECKLIST.md](docs/en/VALIDATION_CHECKLIST.md) for detailed steps on setting up your environment, compiling, and running benchmarks.

#### Windows
```powershell
./build.bat --clean --test
```

#### Linux
```bash
./build.sh --clean --test
```

## Branching Model
- `main` / `master`: Production-ready, stable releases.
- `develop` (if present): Active integration branch.
- Feature branches: `feature/your-feature-name` branched from `develop` or `main`.
