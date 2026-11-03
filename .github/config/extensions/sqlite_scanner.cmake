duckdb_extension_load(sqlite_scanner
        LOAD_TESTS
        GIT_URL https://github.com/duckdb/duckdb-sqlite
        GIT_TAG e56ef1ea965e02c73e2ab082b936235fe5874fb3
        SUBMODULES database-connector
        )
# Static linking on windows does not properly work due to symbol collision
if (NOT WIN32)
    duckdb_extension_statically_link(sqlite_scanner)
endif()
