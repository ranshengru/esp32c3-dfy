Official LZ4 v1.10.0 (`lz4.c`, `lz4.h`) from https://github.com/lz4/lz4

The firmware only calls `LZ4_decompress_safe`. The packer uses the system
liblz4 to write independent 16-line blocks. License: BSD 2-Clause, see LICENSE.
