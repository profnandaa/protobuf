
workspace(name = "upb")

load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")
load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load(":repository_defs.bzl", "bazel_version_repository")

bazel_version_repository(
    name = "bazel_version"
)

http_archive(
    name = "lua",
    build_file = "//:lua.BUILD",
    sha256 = "b9e2e4aad6789b3b63a056d442f7b39f0ecfca3ae0f1fc0ae4e9614401b69f4b",
    strip_prefix = "lua-5.2.4",
    urls = [
        "https://mirror.bazel.build/www.lua.org/ftp/lua-5.2.4.tar.gz",
        "https://www.lua.org/ftp/lua-5.2.4.tar.gz",
    ],
)

git_repository(
    name = "com_google_protobuf",
    #remote = "https://github.com/protocolbuffers/protobuf.git",
    #commit = "78ca77ac8799f67fda7b9a01cc691cd9fe526f25",
    remote = "https://github.com/haberman/protobuf.git",
    commit = "c659a4a4db2e27463e51c732df25730973956be2",
)

http_archive(
    name = "zlib",
    build_file = "@com_google_protobuf//:third_party/zlib.BUILD",
    sha256 = "c3e5e9fdd5004dcb542feda5ee4f0ff0744628baf8ed2dd5d66f8ca1197cb1a1",
    strip_prefix = "zlib-1.2.11",
    urls = ["https://zlib.net/zlib-1.2.11.tar.gz"],
)

git_repository(
    name = "absl",
    commit = "070f6e47b33a2909d039e620c873204f78809492",
    remote = "https://github.com/abseil/abseil-cpp.git",
    shallow_since = "1541627663 -0500"
)

http_archive(
    name = "ragel",
    sha256 = "5f156edb65d20b856d638dd9ee2dfb43285914d9aa2b6ec779dac0270cd56c3f",
    build_file = "//:ragel.BUILD",
    strip_prefix = "ragel-6.10",
    urls = ["http://www.colm.net/files/ragel/ragel-6.10.tar.gz"],
)

http_archive(
   name = "bazel_skylib",
   strip_prefix = "bazel-skylib-master",
   urls = ["https://github.com/bazelbuild/bazel-skylib/archive/master.tar.gz"],
)
