load("@compdb//:aspects.bzl", "compilation_database")

compilation_database(
    name = "compdb",
    testonly = True,
    exec_root_marker = True,
    targets = [
        "//test:hello-test",
        "//main:hello-world",
        "//main:hello-greet",
    ],
)
