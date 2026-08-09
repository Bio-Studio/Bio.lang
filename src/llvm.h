#ifndef BIO_LLVM_H
#define BIO_LLVM_H

/* LLVM backend: bio llvm <file> [-o out] */
int bio_llvm_compile(const char *file, const char *out);

#endif
