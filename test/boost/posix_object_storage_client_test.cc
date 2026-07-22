/*
 * Copyright (C) 2026-present ScyllaDB
 */

/*
 * SPDX-License-Identifier: LicenseRef-ScyllaDB-Source-Available-1.1
 */

#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>

#include <boost/test/unit_test.hpp>

#include <seastar/core/file.hh>
#include <seastar/core/fstream.hh>
#include <seastar/core/iostream.hh>
#include <seastar/core/seastar.hh>
#include <seastar/core/semaphore.hh>
#include <seastar/core/temporary_buffer.hh>
#include <seastar/util/defer.hh>

#include "test/lib/scylla_test_case.hh"
#include "test/lib/tmpdir.hh"

#include "db/object_storage_endpoint_param.hh"
#include "sstables/object_storage_client.hh"
#include "utils/lister.hh"
#include "utils/memory_data_sink.hh"
#include "utils/upload_progress.hh"

namespace fs = std::filesystem;

using namespace sstables;
using param = db::object_storage_endpoint_param;

static shared_ptr<object_storage_client> make_client(const fs::path& base) {
    static thread_local seastar::semaphore memory{std::numeric_limits<size_t>::max()};
    param ep{param::posix_storage{base}};
    // The posix backend ignores the memory semaphore and shard client factory.
    return make_object_storage_client(ep, memory, [](std::string) { return shared_ptr<object_storage_client>{}; });
}

static void put_string(object_storage_client& c, const object_name& name, std::string_view data) {
    memory_data_sink_buffers bufs;
    bufs.push_back(temporary_buffer<char>(data.data(), data.size()));
    c.put_object(name, std::move(bufs)).get();
}

static std::string read_all(object_storage_client& c, const object_name& name) {
    auto in = input_stream<char>(c.make_download_source(name));
    std::string out;
    while (true) {
        auto buf = in.read().get();
        if (buf.empty()) {
            break;
        }
        out.append(buf.get(), buf.size());
    }
    in.close().get();
    return out;
}

// A non-existent base path is rejected when the object storage client is
// created, validating the mount up-front. This check moved here from endpoint
// decoding (config parsing), which no longer touches the filesystem.
SEASTAR_THREAD_TEST_CASE(test_posix_client_rejects_missing_path) {
    tmpdir dir;
    auto missing = dir.path() / "does" / "not" / "exist";

    BOOST_REQUIRE_THROW(make_client(missing), std::invalid_argument);
}

// The factory returns a working posix client, and objects land at
// <path>/<bucket>/<object> with parent directories created on demand.
SEASTAR_THREAD_TEST_CASE(test_posix_put_get_and_layout) {
    tmpdir dir;
    auto base = fs::canonical(dir.path());
    auto client = make_client(base);
    auto stop = defer([&] { client->close().get(); });

    BOOST_REQUIRE(client != nullptr);

    object_name name("bkt", "a/b/obj");
    put_string(*client, name, "hello world");

    BOOST_REQUIRE(file_exists((base / "bkt" / "a" / "b" / "obj").native()).get());
    BOOST_REQUIRE_EQUAL(read_all(*client, name), "hello world");

    BOOST_REQUIRE(client->object_exists(name).get());
    BOOST_REQUIRE(!client->object_exists(object_name("bkt", "missing")).get());
    // A prefix directory is not an object.
    BOOST_REQUIRE(!client->object_exists(object_name("bkt", "a/b")).get());
}

// A PUT overwrites an existing object by unlinking it first, so a hardlinked
// copy keeps its original contents (object-storage copy-on-write semantics).
SEASTAR_THREAD_TEST_CASE(test_posix_copy_object_is_independent) {
    tmpdir dir;
    auto base = fs::canonical(dir.path());
    auto client = make_client(base);
    auto stop = defer([&] { client->close().get(); });

    put_string(*client, object_name("bkt", "src"), "original");
    client->copy_object(object_name("bkt", "src"), object_name("bkt", "d/copy")).get();

    BOOST_REQUIRE_EQUAL(read_all(*client, object_name("bkt", "d/copy")), "original");

    // The copy is a hardlink: same inode as the source.
    auto st_src = file_stat((base / "bkt" / "src").native()).get();
    auto st_dst = file_stat((base / "bkt" / "d" / "copy").native()).get();
    BOOST_REQUIRE_EQUAL(st_src.inode_number, st_dst.inode_number);

    // Overwriting the source must not affect the copy.
    put_string(*client, object_name("bkt", "src"), "changed");
    BOOST_REQUIRE_EQUAL(read_all(*client, object_name("bkt", "src")), "changed");
    BOOST_REQUIRE_EQUAL(read_all(*client, object_name("bkt", "d/copy")), "original");
}

// Deleting an object is idempotent and prunes the now-empty parent directories
// up to (but not including) the bucket root.
SEASTAR_THREAD_TEST_CASE(test_posix_delete_is_idempotent_and_prunes) {
    tmpdir dir;
    auto base = fs::canonical(dir.path());
    auto client = make_client(base);
    auto stop = defer([&] { client->close().get(); });

    put_string(*client, object_name("bkt", "x/y/z"), "d");
    client->delete_object(object_name("bkt", "x/y/z")).get();

    BOOST_REQUIRE(!file_exists((base / "bkt" / "x" / "y" / "z").native()).get());
    BOOST_REQUIRE(!file_exists((base / "bkt" / "x" / "y").native()).get());
    BOOST_REQUIRE(!file_exists((base / "bkt" / "x").native()).get());
    // The bucket directory itself survives an empty listing.
    BOOST_REQUIRE(file_exists((base / "bkt").native()).get());

    // Deleting a missing object is a no-op.
    client->delete_object(object_name("bkt", "x/y/z")).get();
}

// Pruning stops at the first non-empty ancestor: a sibling object keeps the
// shared prefix directory alive.
SEASTAR_THREAD_TEST_CASE(test_posix_delete_prune_stops_at_nonempty) {
    tmpdir dir;
    auto base = fs::canonical(dir.path());
    auto client = make_client(base);
    auto stop = defer([&] { client->close().get(); });

    put_string(*client, object_name("bkt", "p/a"), "1");
    put_string(*client, object_name("bkt", "p/q/b"), "2");
    client->delete_object(object_name("bkt", "p/q/b")).get();

    BOOST_REQUIRE(!file_exists((base / "bkt" / "p" / "q").native()).get());
    BOOST_REQUIRE(file_exists((base / "bkt" / "p").native()).get());
    BOOST_REQUIRE(file_exists((base / "bkt" / "p" / "a").native()).get());
}

// Listing walks the prefix directory recursively and returns names relative to
// the prefix, applying the caller's filter.
SEASTAR_THREAD_TEST_CASE(test_posix_lister_strips_prefix) {
    tmpdir dir;
    auto base = fs::canonical(dir.path());
    auto client = make_client(base);
    auto stop = defer([&] { client->close().get(); });

    put_string(*client, object_name("bkt", "tab/f1"), "a");
    put_string(*client, object_name("bkt", "tab/f2"), "b");
    put_string(*client, object_name("bkt", "tab/sub/f3"), "c");
    put_string(*client, object_name("bkt", "other/f4"), "d");

    auto collect = [] (abstract_lister lister) {
        std::set<std::string> names;
        while (auto de = lister.get().get()) {
            names.insert(de->name);
        }
        lister.close().get();
        return names;
    };

    auto all = collect(client->make_object_lister("bkt", "tab/",
            [] (const fs::path&, const directory_entry&) { return true; }));
    BOOST_REQUIRE((all == std::set<std::string>{"f1", "f2", "sub/f3"}));

    auto filtered = collect(client->make_object_lister("bkt", "tab/",
            [] (const fs::path&, const directory_entry& e) { return e.name != "f2"; }));
    BOOST_REQUIRE((filtered == std::set<std::string>{"f1", "sub/f3"}));

    // Listing a non-existent prefix yields an empty result, not an error.
    auto empty = collect(client->make_object_lister("bkt", "nope/",
            [] (const fs::path&, const directory_entry&) { return true; }));
    BOOST_REQUIRE(empty.empty());
}

// Relative path traversal that would escape the bucket is rejected at every
// entry point.
SEASTAR_THREAD_TEST_CASE(test_posix_rejects_path_traversal) {
    tmpdir dir;
    auto base = fs::canonical(dir.path());
    auto client = make_client(base);
    auto stop = defer([&] { client->close().get(); });

    BOOST_REQUIRE_THROW(put_string(*client, object_name("bkt", "../evil"), "x"), std::invalid_argument);
    BOOST_REQUIRE_THROW(client->object_exists(object_name("bkt", "a/../../b")).get(), std::invalid_argument);
    BOOST_REQUIRE_THROW(client->make_readable_file(object_name("bkt", "../../etc/passwd")), std::invalid_argument);
    BOOST_REQUIRE_THROW(client->make_upload_sink(object_name("bkt", "../x")), std::invalid_argument);
    BOOST_REQUIRE_THROW(client->copy_object(object_name("bkt", "ok"), object_name("bkt", "../nope")).get(), std::invalid_argument);

    // A sneaky escape into a sibling bucket is also rejected.
    BOOST_REQUIRE_THROW(client->make_readable_file(object_name("bkt", "../bkt2/secret")), std::invalid_argument);
}

// put_object accepts multiple buffers spanning several DMA blocks and stores
// them contiguously.
SEASTAR_THREAD_TEST_CASE(test_posix_put_object_multi_buffer) {
    tmpdir dir;
    auto base = fs::canonical(dir.path());
    auto client = make_client(base);
    auto stop = defer([&] { client->close().get(); });

    std::string expected;
    memory_data_sink_buffers bufs;
    for (int i = 0; i < 5; ++i) {
        std::string chunk(50 * 1024, char('a' + i));
        expected += chunk;
        bufs.push_back(temporary_buffer<char>(chunk.data(), chunk.size()));
    }
    client->put_object(object_name("bkt", "big/obj"), std::move(bufs)).get();

    BOOST_REQUIRE_EQUAL(read_all(*client, object_name("bkt", "big/obj")).size(), expected.size());
    BOOST_REQUIRE(read_all(*client, object_name("bkt", "big/obj")) == expected);
}

// A deferred upload sink creates the object lazily on first use.
SEASTAR_THREAD_TEST_CASE(test_posix_upload_sink) {
    tmpdir dir;
    auto base = fs::canonical(dir.path());
    auto client = make_client(base);
    auto stop = defer([&] { client->close().get(); });

    auto sink = client->make_upload_sink(object_name("bkt", "via/sink"));
    sink.put(temporary_buffer<char>("abc", 3)).get();
    sink.put(temporary_buffer<char>("def", 3)).get();
    sink.flush().get();
    sink.close().get();

    BOOST_REQUIRE_EQUAL(read_all(*client, object_name("bkt", "via/sink")), "abcdef");
}

// upload_file streams a local file into an object and reports progress.
SEASTAR_THREAD_TEST_CASE(test_posix_upload_file) {
    tmpdir dir;
    tmpdir src_dir;
    auto base = fs::canonical(dir.path());
    auto client = make_client(base);
    auto stop = defer([&] { client->close().get(); });

    const std::string content(100 * 1024, 'x');
    auto local = src_dir.path() / "local-source-file";
    {
        auto f = open_file_dma(local.native(), open_flags::wo | open_flags::create | open_flags::truncate).get();
        auto os = make_file_output_stream(std::move(f)).get();
        os.write(content.data(), content.size()).get();
        os.flush().get();
        os.close().get();
    }

    utils::upload_progress up;
    client->upload_file(local, object_name("bkt", "uploaded"), up).get();

    BOOST_REQUIRE_EQUAL(read_all(*client, object_name("bkt", "uploaded")), content);
    BOOST_REQUIRE_EQUAL(up.total, content.size());
    BOOST_REQUIRE_EQUAL(up.uploaded, content.size());
}

// A dup()'d file_handle reopens the same object from scratch, on to_file().
SEASTAR_THREAD_TEST_CASE(test_posix_readable_file_dup) {
    tmpdir dir;
    auto base = fs::canonical(dir.path());
    auto client = make_client(base);
    auto stop = defer([&] { client->close().get(); });

    const std::string content = "duplicate me";
    put_string(*client, object_name("bkt", "dup/obj"), content);

    auto f = client->make_readable_file(object_name("bkt", "dup/obj"));
    auto handle = f.dup();
    auto handle_copy = handle; // exercises file_handle_impl::clone()
    f.close().get();

    auto f2 = std::move(handle).to_file();
    BOOST_REQUIRE_EQUAL(f2.size().get(), content.size());
    auto buf = f2.dma_read_bulk<char>(0, content.size()).get();
    BOOST_REQUIRE_EQUAL(std::string_view(buf.get(), buf.size()), content);
    f2.close().get();

    // The cloned handle is independent and still usable after the original
    // was consumed above.
    auto f3 = handle_copy.to_file();
    BOOST_REQUIRE_EQUAL(f3.size().get(), content.size());
    f3.close().get();
}

// allocate()/discard() are no-ops on a read-only object file.
SEASTAR_THREAD_TEST_CASE(test_posix_readable_file_allocate_discard_are_noops) {
    tmpdir dir;
    auto base = fs::canonical(dir.path());
    auto client = make_client(base);
    auto stop = defer([&] { client->close().get(); });

    put_string(*client, object_name("bkt", "noop/obj"), "hello");

    auto f = client->make_readable_file(object_name("bkt", "noop/obj"));
    BOOST_REQUIRE_NO_THROW(f.allocate(0, 4096).get());
    BOOST_REQUIRE_NO_THROW(f.discard(0, 4096).get());
    // The file must still be fully readable afterwards.
    BOOST_REQUIRE_EQUAL(f.size().get(), 5u);
    auto buf = f.dma_read_bulk<char>(0, 5).get();
    BOOST_REQUIRE_EQUAL(std::string_view(buf.get(), buf.size()), "hello");
    f.close().get();
}

// make_download_source() closes its backing file on close().
SEASTAR_THREAD_TEST_CASE(test_posix_download_source_closes_cleanly) {
    tmpdir dir;
    auto base = fs::canonical(dir.path());
    auto client = make_client(base);
    auto stop = defer([&] { client->close().get(); });

    put_string(*client, object_name("bkt", "dl/obj"), "content");

    for (int i = 0; i < 50; ++i) {
        BOOST_REQUIRE_EQUAL(read_all(*client, object_name("bkt", "dl/obj")), "content");
    }
}
