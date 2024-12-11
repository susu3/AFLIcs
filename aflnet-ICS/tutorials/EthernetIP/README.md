# OpENer AFL Fuzzing Setup Guide

This guide helps you set up OpENer for fuzzing with AFL (American Fuzzy Lop).

## 1. Clone OpENer

```bash
git clone https://github.com/EIPStackGroup/OpENer
```

## 2. Replace Configuration Files

Copy the following files from this repository to your OpENer clone:

```bash
# Replace setup script
cp setup_posix_fuzz_afl.sh OpENer/bin/posix/

# Replace CMakeLists.txt
cp CMakeLists.txt OpENer/source/src/ports/POSIX/
```

## 3. Set Up Environment

Add the following to your `~/.bashrc` or run them in your terminal:

```bash
# Set AFL environment variables
export AFL_PATH=/path/to/your/AFLIcs/aflnet-ICS
export AFL_CC=$(which clang)
export AFL_CXX=$(which clang++)
export PATH=$AFL_PATH:$PATH

# Optional but recommended
export AFL_USE_ASAN=1  # Enable Address Sanitizer
export AFL_INST_RATIO=100  # Full instrumentation
```

Don't forget to source your bashrc if modified:

```bash
source ~/.bashrc
```

## 4. Build OpENer

```bash
cd OpENer/bin/posix
./setup_posix_fuzz_afl.sh
make -j$(nproc)
```

## 5. Verify Build

```bash
# Check if OpENer binary exists
find . -name OpENer
```

## Troubleshooting

### Common Issues:
1. **Missing AFL**: Ensure AFL is properly installed and in your PATH
   ```bash
   which afl-clang-fast
   ```

2. **Compiler Errors**: Make sure you have LLVM/Clang installed
   ```bash
   sudo apt-get install clang llvm
   ```

3. **Link Errors**: Check if all libraries are properly linked
   ```bash
   ldd ./src/ports/POSIX/OpENer
   ```

### Build System:
- Clean build directory if you encounter issues:
  ```bash
  cd OpENer/bin/posix
  rm -rf *
  ./setup_posix_fuzz_afl.sh
  make -j$(nproc)
  ```

## File Changes

1. `bin/posix/setup_posix_fuzz_afl.sh`:
   - Modified to use AFL compiler
   - Added proper compiler flags

2. `source/src/ports/POSIX/CMakeLists.txt`:
   - Fixed library link order

