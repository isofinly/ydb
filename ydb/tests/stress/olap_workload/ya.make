PY3_PROGRAM(olap_workload)

PY_SRCS(
    __main__.py
)

PEERDIR(
    ydb/tests/stress/common
    ydb/public/sdk/python
    ydb/public/sdk/python/enable_v3_new_behavior
    library/python/monlib
)

END()

RECURSE_FOR_TESTS(
    tests
)
