import asyncio
import logging
import os
import shutil
from test.cluster.util import new_test_keyspace, FeatureConfig, feature_configs, FeatureConfigurations
from test.pylib.scylla_cluster_manager import ScyllaClusterManager
from test.pylib.util import unique_name
import pytest

logger = logging.getLogger(__name__)

@pytest.mark.skip_mode(mode='release', reason='error injections are not supported in release mode')
async def test_drop_table_during_streaming_receiver_side(manager: ScyllaClusterManager):
    servers = [await manager.server_add(config={
        'error_injections_at_startup': ['stream_mutation_fragments_table_dropped'],
        'enable_repair_based_node_ops': False,
        'enable_user_defined_functions': False,
        'tablets_mode_for_new_keyspaces': 'disabled'
    }) for _ in range(2)]


@pytest.mark.parametrize("feature_config", feature_configs(FeatureConfigurations.EVENTUAL_CONSISTENCY,
    FeatureConfigurations.STRONG_CONSISTENCY, FeatureConfigurations.LOGSTOR_EVENTUAL_CONSISTENCY,
    FeatureConfigurations.LOGSTOR_STRONG_CONSISTENCY))
@pytest.mark.skip_mode(mode='release', reason='error injections are not supported in release mode')
async def test_drop_table_during_flush(manager: ScyllaClusterManager, feature_config: FeatureConfig):
    servers = [await manager.server_add(config=feature_config.get_cluster_cfg({})) for _ in range(2)]

    await manager.api.enable_injection(servers[0].ip_addr, "flush_tables_on_all_shards_table_drop", True)
    keyspace_opts = feature_config.get_keyspace_opts(
        "WITH replication = {'class': 'NetworkTopologyStrategy', 'replication_factor': 1}")

    cql = manager.get_cql()

    async with new_test_keyspace(manager, keyspace_opts) as ks:
        await cql.run_async(feature_config.get_table_opts(f"CREATE TABLE {ks}.test (pk int PRIMARY KEY, c int);"))
        await asyncio.gather(*[cql.run_async(f"INSERT INTO {ks}.test (pk, c) VALUES ({k}, {k%3});") for k in range(64)])
        await manager.api.keyspace_flush(servers[0].ip_addr, ks, "test")


@pytest.mark.parametrize("feature_config", feature_configs(FeatureConfigurations.EVENTUAL_CONSISTENCY,
    FeatureConfigurations.STRONG_CONSISTENCY))
@pytest.mark.skip_mode(mode='release', reason='error injections are not supported in release mode')
async def test_drop_table_during_load_and_stream(manager: ScyllaClusterManager, feature_config: FeatureConfig):
    """Verify that dropping a table while load_and_stream is in progress
    neither crashes the node nor waits for the streaming to finish.

    The 'load_and_stream_before_streaming_batch' injection parks
    load_and_stream inside its streaming loop, where it holds both a
    replica::table& and the stream_in_progress() phaser guard.  The pause is
    never released, so the DROP can only complete if it aborted the streaming
    itself.

    A single node with a single shard is enough, and it keeps the outcome
    deterministic: the only load_and_stream fiber is already parked when the
    drop happens, so the streaming always fails with the abort rather than
    with a missing column family.
    """
    server = await manager.server_add(config=feature_config.get_cluster_cfg({}), cmdline=['--smp', '1'])

    cql = manager.get_cql()

    ks = unique_name("ks_")
    cf = "test"
    keyspace_opts = feature_config.get_keyspace_opts(
        f"CREATE KEYSPACE {ks} WITH replication = {{'class': 'NetworkTopologyStrategy', 'replication_factor': 1}}")
    table_opts = feature_config.get_table_opts(f"CREATE TABLE {ks}.{cf} (pk int PRIMARY KEY, c int)")

    await cql.run_async(keyspace_opts)
    try:
        await cql.run_async(table_opts)

        # Insert data and flush to create SSTables on disk
        await asyncio.gather(*[cql.run_async(f"INSERT INTO {ks}.{cf} (pk, c) VALUES ({k}, {k})") for k in range(64)])
        await manager.api.flush_keyspace(server.ip_addr, ks)

        # Take snapshot so we have SSTables to copy into upload dir
        snap_name = unique_name("snap_")
        await manager.api.take_snapshot(server.ip_addr, ks, snap_name)

        # Copy snapshot SSTables into the upload directory.
        # load_and_stream will read these and stream to replicas.
        workdir = await manager.server_get_workdir(server.server_id)
        cf_dir = os.listdir(f"{workdir}/data/{ks}")[0]
        cf_path = os.path.join(f"{workdir}/data/{ks}", cf_dir)
        upload_dir = os.path.join(cf_path, "upload")
        os.makedirs(upload_dir, exist_ok=True)

        snapshots_dir = os.path.join(cf_path, "snapshots", snap_name)
        exclude_list = ["manifest.json", "schema.cql"]
        for item in os.listdir(snapshots_dir):
            if item not in exclude_list:
                shutil.copy2(os.path.join(snapshots_dir, item), os.path.join(upload_dir, item))

        await manager.api.enable_injection(server.ip_addr, "load_and_stream_before_streaming_batch", one_shot=False)
        server_log = await manager.server_open_log(server.server_id)
        log_mark = await server_log.mark()

        refresh_task = asyncio.ensure_future(
            manager.api.load_new_sstables(server.ip_addr, ks, cf, load_and_stream=True))
        await manager.api.wait_for_injection_enter(server.ip_addr, "load_and_stream_before_streaming_batch")
        logger.info("load_and_stream paused at injection point")

        # The injection is never released, so this only returns if the drop
        # aborted the streaming. The timeout has to stay well below the
        # injection's own 60s timeout, which would mask a missing abort.
        await asyncio.wait_for(cql.run_async(f"DROP TABLE {ks}.{cf}"), timeout=30)

        with pytest.raises(Exception, match="was dropped"):
            await asyncio.wait_for(refresh_task, timeout=30)

        # SCYLLADB-1352: the streamer holds a replica::table& across the pause,
        # so an abort that outruns the phaser guard would be a use-after-free.
        crash_matches = await server_log.grep(
            r"Segmentation fault|AddressSanitizer|heap-use-after-free|ABORTING",
            from_mark=log_mark)
        assert not crash_matches, "Node crashed during load_and_stream"
    finally:
        # Clean up keyspace if it still exists
        try:
            await cql.run_async(f"DROP KEYSPACE IF EXISTS {ks}")
        except Exception:
            pass
