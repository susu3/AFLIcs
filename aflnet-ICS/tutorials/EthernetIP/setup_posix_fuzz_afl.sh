cmake \
    -DCMAKE_C_COMPILER=$(which afl-clang-fast) \
    -DCMAKE_CXX_COMPILER=$(which afl-clang-fast++) \
    -DCMAKE_C_FLAGS="-fsanitize=address -g" \
    -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
    -DCMAKE_C_LINK_EXECUTABLE="clang <FLAGS> <CMAKE_C_LINK_FLAGS> <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES> /home/zgc/AFLIcs/aflnet-ICS/afl-llvm-rt.o" \
    -DUSE_FUZZ_AFL=ON \
    -DOpENer_PLATFORM:STRING="POSIX" \
    -DCMAKE_BUILD_TYPE:STRING="Debug" \
    -DBUILD_SHARED_LIBS:BOOL=OFF \
    ../../source
