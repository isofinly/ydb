PY3_LIBRARY()

STYLE_PYTHON()

PEERDIR(
    contrib/tools/python3
    contrib/tools/python3/lib2/py
    library/cpp/resource
)

CFLAGS(-DCYTHON_REGISTER_ABCS=0)

NO_PYTHON_INCLUDES()

ENABLE(PYBUILD_NO_PYC)

PY_SRCS(
    entry_points.py
    TOP_LEVEL

    CYTHON_DIRECTIVE
    language_level=3

    __res.pyx
    sitecustomize.pyx
)

IF (EXTERNAL_PY_FILES)
    PEERDIR(
        library/python/runtime_py3/enable_external_py_files
    )
ENDIF()

IF (CYTHON_COVERAGE)
    # Let covarage support add all needed files to resources
ELSE()
    RESOURCE_FILES(
        DONT_COMPRESS
        PREFIX ${MODDIR}/
        __res.pyx
        importer.pxi
        sitecustomize.pyx
    )
ENDIF()

END()

RECURSE(
    enable_external_py_files
)

RECURSE_FOR_TESTS(
    test
)
