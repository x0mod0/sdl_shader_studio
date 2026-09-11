# Turns a text file into a C++ translation unit exposing it as a string literal.
# Used to embed templates/s3pack.h into the core library so the CLI is a single
# self-contained binary with no runtime resource lookup.
#
#   cmake -DINPUT=... -DOUTPUT=... -DSYMBOL=kS3PackTemplate -P tools/embed_file.cmake

if(NOT DEFINED INPUT OR NOT DEFINED OUTPUT OR NOT DEFINED SYMBOL)
    message(FATAL_ERROR "embed_file.cmake requires INPUT, OUTPUT and SYMBOL")
endif()

file(READ "${INPUT}" CONTENT)

# The delimiter must not appear in the file; S3PACKRAW is not valid inside it.
if(CONTENT MATCHES "\\)S3PACKRAW\"")
    message(FATAL_ERROR "raw string delimiter collision in ${INPUT}")
endif()

file(WRITE "${OUTPUT}"
"// Generated from ${INPUT} by tools/embed_file.cmake. Do not edit.\n"
"extern const char ${SYMBOL}[];\n"
"const char ${SYMBOL}[] = R\"S3PACKRAW(${CONTENT})S3PACKRAW\";\n")
