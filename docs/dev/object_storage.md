# Keeping sstables on S3/GS

On of the ways to use object storage is to keep sstables directly on it as objects.

## Enabling the feature

Object-storage backed keyspaces are supported out of the box; no experimental
feature flag is required. Simply configure the object-storage endpoints in
`scylla.yaml` (see below) and create a keyspace with the desired storage
options (see `CREATE KEYSPACE` extensions below).

## Configuring AWS S3 access

You can define endpoint details in the `scylla.yaml` file. For example:
```yaml
object_storage_endpoints:
  - name: https://s3.us-east-1.amazonaws.com:443
    aws_region: us-east-1
```

## Configuring GCP storage access

Similarly to AWS, define endpoint details in `scylla.yaml` like:
```yaml
object_storage_endpoints:
  - name: https://storage.googleapis.com
    type: gs
    credentials_file: <gcp account credentials json file>
```

Typically, google compute storage only uses the same endpoint URI (unless using private proxy or mock
server), so `name` can also use the `default` moniker.

`credentials_file` can be omitted, in which case the default credentials on the machine
will be used, i.e. resolving the current users credentials or fallback to machine credentials
if running on a GCP instance.

If set, the environment variable `GOOGLE_APPLICATION_CREDENTIALS` can be set to point to a 
credentials file. 

If no credentials file is set, the default credentials will be searched, i.e. `application_default_credentials.json`
in the gcp local data folder.

You can also set the `credentials_file` to `none` to completely skip authentication. Useful for testing
on mock servers.


### Local/Development Environment

In a local or development environment, you usually need to set AWS authentication tokens in environment variables to ensure the client works properly. For instance:
```sh
export AWS_ACCESS_KEY_ID=EXAMPLE_ACCESS_KEY_ID
export AWS_SECRET_ACCESS_KEY=EXAMPLE_SECRET_ACCESS_KEY
```

Additionally, you may include an `aws_session_token`, although this is not typically necessary for local or development environments:

```sh
export AWS_ACCESS_KEY_ID=EXAMPLE_ACCESS_KEY_ID
export AWS_SECRET_ACCESS_KEY=EXAMPLE_SECRET_ACCESS_KEY
export AWS_SESSION_TOKEN=EXAMPLE_TEMPORARY_SESSION_TOKEN
```

For gs, when using local mock server, authentication is normally not used.

### Important Note

The examples above are intended for development or local environments. You should *never* use this approach in production. The Scylla S3 client will first attempt to access credentials from environment variables. If it fails to obtain credentials, it will then try to retrieve them from the AWS Security Token Service (STS) or the EC2 Instance Metadata Service.

For the EC2 Instance Metadata Service to function correctly, no additional configuration is required. However, STS requires the IAM Role ARN to be defined in the `scylla.yaml` file, as shown below:
```yaml
object_storage_endpoints:
  - name: https://s3.us-east-1.amazonaws.com:443
    aws_region: us-east-1
    iam_role_arn: arn:aws:iam::123456789012:instance-profile/my-instance-instance-profile
```

## Creating keyspace with S3

Sstables location is keyspace-scoped. In order to create a keyspace with S3
storage use `CREATE KEYSPACE` with `STORAGE = { 'type': 'S3', 'endpoint': '$endpoint_name', 'bucket': '$bucket' }`
parameters, where `$endpoint_name` should match with the corresponding `name`
of the configured endpoint in the YAML file above.

In the following example, an endpoint named "s3.us-east-2.amazonaws.com" is
defined in `scylla.yaml`, and this endpoint is used when creating the
keyspace "ks".

in `scylla.yaml`:

```yaml
object_storage_endpoints:
  - name: https://s3.us-east-2.amazonaws.com:443
    aws_region: us-east-2
```

and when creating the keyspace:

```cql
CREATE KEYSPACE ks
  WITH REPLICATION = {
   'class' : 'NetworkTopologyStrategy',
   'replication_factor' : 1
  }
  AND STORAGE = {
   'type' : 'S3',
   'endpoint' : 's3.us-east-2.amazonaws.com',
   'bucket' : 'bucket-for-testing'
  };
```


## Creating keyspace with GS

This mirrors AWS S3 config.

in `scylla.yaml`:

```yaml
object_storage_endpoints:
  - name: default
    credentials_file: <credentials file>|none
```

and when creating the keyspace:

```cql
CREATE KEYSPACE ks
  WITH REPLICATION = {
   'class' : 'NetworkTopologyStrategy',
   'replication_factor' : 1
  }
  AND STORAGE = {
   'type' : 'GS',
   'endpoint' : 'default',
   'bucket' : 'bucket-for-testing'
  };
```

# Copying sstables on S3/GS (backup)

It's possible to upload sstables from data/ directory on S3 via API. This is good
to do because in that case all the resources that are needed for that operation (like
disk IO bandwidth and IOPS, CPU time, networking bandwidth) will be under Seastar's
control and regular Scylla workload will not be randomly affected.

The API endpoint name is `/storage_service/backup` and its Swagger description can be
found [here](../../api/api-doc/storage_service.json). Accepted parameters are

* *keyspace*: the keyspace to copy sstables from
* *table*: the table to copy sstables from
* *snapshot*: the snapshot name to copy sstables from
* *endpoint*: the key in the object storage configuration file. Can be either an AWS or GCP endpoint
* *bucket*: bucket name to put sstables' files in
* *prefix*: prefix to put sstables' files under

Currently only snapshot backup is possible, so first one needs to take [snapshot](../kb/snapshots.rst)

One table is uploaded per call, the destination object names will look like
`s3://bucket/some/prefix/to/store/data/.../sstable`
or 
`gs://bucket/some/prefix/to/store/data/.../sstable`

# System tables
There are a few system tables that object storage related code needs to touch in order to operate.
* [system_distributed.snapshot_sstables](./snapshot_sstables.md) - Used during restore by worker nodes to get the list of SSTables that need to be downloaded from object storage and restored locally.
* [system.sstables](./system_keyspace.md#systemsstables) - Used to keep track of SSTables on object storage when a keyspace is created with object storage storage_options.

# Manipulating S3 data

This section intends to give an overview of where, when and how we store data in S3 and provide a quick set of commands  
which help gain local access to the data in case there is a need for manual intervention.

Most of the time it won't be necessary to touch the data on S3 directly, there are transparent REST APIs and Scylla Manager  
commands for backup and restore and Scylla can operate normally with S3 storage configured in the  
`CREATE KEYSPACE` cql documented at [ScyllaDB CQL Extensions](../cql/cql-extensions.md#keyspace-storage-options).  

However, if for some reason the SSTables become corrupted and need an offline scrub before re-uploading  
or if a bug investigation leads to the need to analyze the backup data, follow the information below to access  
that data.  

Issue tracking the document [here](https://github.com/scylladb/scylladb/issues/22438).

## Object Storage Layout

There are currently four mechanisms in Scylla which write data to S3/GS:

1. Scylla Manager backup

When performing a backup with `sctool`, a `backup` prefix is created within the bucket passed as argument and  
under that prefix, Scylla Manager stores all the backup data of all the backup tasks organized by cluster name,  
datacenter, keyspace, etc.

Follow [Specification | ScyllaDB Docs](https://manager.docs.scylladb.com/stable/backup/specification.html) in the Scylla Manager documentation for the exact layout  
under the `backup` prefix.

2. `/storage_service/backup` REST API

When using the `/storage_service/backup` REST API, the data is stored under the prefix passed as argument to the API.  
The structure under this prefix is identical to what you’d find in the typical Scylla snapshot.  
There is a manifest file which contains the list of Data files for each SSTable, the schema file and all the SSTables  
components stored flat under the prefix.
```perl
scylla-bucket/prefix/
│
├── manifest.json
├── schema.cql
|
├── me-3gqe_1lnj_4sbpc2ezoscu9hhtor-big-Data.db
├── me-3gqe_1lnj_4sbpc2ezoscu9hhtor-big-Index.db
├── me-3gqe_1lnj_4sbpc2ezoscu9hhtor-big-Summary.db
├── ...
│
├── ma-1abx_k29m_9fyug3sdtjwj8krpqh-big-Data.db
├── ma-1abx_k29m_9fyug3sdtjwj8krpqh-big-Index.db
├── ma-1abx_k29m_9fyug3sdtjwj8krpqh-big-Summary.db
├── ...
│
└── ... (more SSTable components)
```
See the API [documentation](#copying-sstables-on-s3-backup) for more details about the actual backup request.

3. `/storage_service/tablets/backup` REST API (cluster backup)

When using the `/storage_service/tablets/backup` REST API, one backup location is given per
datacenter, and the data of every datacenter which maps to a location is stored under the
prefix of that location. Unlike the `/storage_service/backup` API, the manifest and the
component files are stored under two different prefixes: the manifest under
`{prefix}/snapshots/{snapshot_name}/manifest.json` and the component files under
`{prefix}/sstables/{sstable_id}/`. The component files keep the names they have in the local
snapshot directory, so a component object is named
`{prefix}/sstables/{sstable_id}/me-<generation>-big-Data.db`. A reference object
`{prefix}/sstables/{sstable_id}/refs/snapshot-{snapshot_name}/{generation}` records which
snapshot the component files were uploaded for.

4. `CREATE KEYSPACE` with S3/GS storage

When creating a keyspace with S3/GS storage, the data is stored under the bucket passed as argument to the `CREATE KEYSPACE` statement.
Once the statement is issued, Scylla will transparently use the S3/GS bucket as the location of the SSTables for that keyspace.

Automatically managed object-storage tables use a unified layout shared across S3 and GS. SSTable components are stored under a static `sstables/` prefix and grouped by the stable SSTable identifier (`sstable_id`), not by the node-local generation name:

```perl
scylla-sstables-bucket/
│
└── sstables/
    ├── 4f4d0a90-d8c6-11f0-8b18-060de9f3bd1b/
    │   ├── Data.db
    │   ├── Index.db
    │   ├── Summary.db
    │   ├── TOC.txt
    │   ├── ...
    │   └── refs/
    │       └── nodes/
    │           ├── 7adf1ca2-6783-40ab-aa1f-ef1e0b5d98ba/
    │           │   └── 3gqe_1lnj_4sbpc2ezoscu9hhtor
    │           └── 10e68be1-d352-4173-bf34-2249dbb7329e/
    │               └── 4c5s_0281_0v5kg2b4gri84iggoz
    ├── 87a72290-d8c6-11f0-a5ea-060de9f3bd1b/
    │   ├── Data.db
    │   ├── Index.db
    │   ├── Summary.db
    │   ├── TOC.txt
    │   ├── ...
    │   └── refs/
    │       └── nodes/
    │           └── 7adf1ca2-6783-40ab-aa1f-ef1e0b5d98ba/
    │               └── 5n8a_03tw_2jv4g2a4k2m33sq8ah
    └── ...
```

The managed SSTable prefix is:

```text
sstables/
```

Each SSTable lives under:

```text
sstables/{sstable_id}/
```

SSTable component object names are the component suffixes used by the local SSTable format, for example `Data.db`, `Index.db`, `Summary.db`, `Scylla.db`, and `TOC.txt`.

Reference objects under `refs/nodes/` record which nodes still own a reference to the SSTable data:

```text
sstables/{sstable_id}/refs/nodes/{host_id}/{generation}
```

The reference object body is empty. Its name is the metadata: `{host_id}` identifies the node and `{generation}` is that node's local SSTable generation name for the shared SSTable.

The `sstable_id` identifies the shared object-storage SSTable data. The local `generation` identifies a node-local SSTable entry in `system.sstables`. A newly created SSTable normally has an `sstable_id` derived from its generation. After tablet migration or reference sharing, multiple local SSTable entries can have different generations while pointing at the same object-storage data via the same `sstable_id`.

Object-storage SSTable lifecycle:
- Creation: Scylla uploads component objects under `{sstable_id}` and creates this node's `refs/nodes/{host_id}/{generation}` reference before sealing the local row in `system.sstables`.
- Sharing: when tablet migration can share object-storage data, the receiving node creates a new local SSTable entry with its own generation and adds a node reference under the existing `{sstable_id}` prefix instead of copying all component objects.
- Local removal: when a node removes its local SSTable, it first deletes its own reference object. If other references remain, component objects are left intact.
- Final cleanup: component objects are deleted only after no reference objects remain for the `sstable_id`. This prevents one node from deleting shared data still referenced by another node.

The `status` and `state` fields in `system.sstables` describe the local SSTable entry lifecycle. They do not describe a global lifecycle state for the object-storage component set identified by `sstable_id`.

### The snapshot manifest

A snapshot is described by a manifest.json file. Two writers produce one and they fill
different members, so a reader has to read `manifest.scope` first and interpret the rest of the
file accordingly. A member which the writer does not fill is absent from the file, it is not
present with a null value.

- Scope `node` is written by a local snapshot, one manifest per node, into the table snapshot
  directory. The `/storage_service/backup` API does not write a manifest of its own: it uploads
  the one the local snapshot wrote, next to the component files, under the prefix passed to the
  API.
- Scope `dc` is written by cluster backup, one manifest per backup location. Several
  datacenters can be backed up to the same location, in which case one manifest describes all
  of them and the `nodes` member lists the nodes of all of them. The manifest.json file is
  stored under `{prefix}/snapshots/{snapshot_name}/` while the component files are stored under
  `{prefix}/sstables/{sstable_id}/`, see the cluster backup layout above.

Cluster backup does not store every replica of a token range. Of the SSTables which belong to
the repaired set of a tablet it stores those of a single node only, so for such a token range a
manifest with scope `dc` describes one copy of the data and the rack an SSTable was backed up
from does not identify a replica of the restored table. SSTables which are not in the repaired
set are stored from every node which owns them. The replication factor is not recorded in the
manifest.

The json structure is as follows:

```json
{
  "manifest": {
    "version": "1.0",
    "scope": "node|dc"
  },
  "node": {
    "host_id": "<UUID>",
    "datacenter": "mydc",
    "rack": "myrack"
  },
  "nodes": [
    {
      "host_id": "<UUID>",
      "datacenter": "mydc",
      "rack": "myrack"
    }
  ],
  "snapshot": {
    "name": "snapshot name",
    "created_at": 1767225600,
    "expires_at": 0
  },
  "table": {
    "keyspace_name": "my_keyspace",
    "table_name": "my_table",
    "table_id": "<UUID>",
    "tablets_type": "none|powof2|arbitrary",
    "tablet_count": 4
  },
  "tablets": [
    {
      "id": 0,
      "first_token": -9223372036854775808,
      "last_token": -4611686018427387905,
      "repair_time": 0,
      "repaired_at": 0
    }
  ],
  "sstables": [
    {
      "id": "67e35000-d8c6-11f0-9599-060de9f3bd1b",
      "toc_name": "me-3gw7_0ndy_3wlq829wcsddgwha1n-big-TOC.txt",
      "data_size": 75,
      "index_size": 8,
      "first_token": -8629266958227979430,
      "last_token": 9168982884335614769,
      "tablet_id": 0,
      "repaired_at": 0,
      "node": "<UUID>"
    }
  ]
}
```

The `manifest` member contains the following attributes:
- `version` - representing the version of the manifest itself. It is incremented when members are added or removed from the manifest.
- `scope` - the scope of metadata stored in this manifest file.  The following scopes are supported:
    - `node` - the manifest describes all SSTables owned by this node in this snapshot.
    - `dc` - the manifest describes the SSTables backed up from one datacenter. Written by cluster backup.

The `node` member contains metadata about the node the manifest describes, which enables datacenter- or rack-aware restore. Written for scope `node` only.
- `host_id` - is the node's unique host_id (a UUID).
- `datacenter` - is the node's datacenter.
- `rack` - is the node's rack.

The `nodes` member is a list containing the same metadata about every node of the datacenter the manifest describes. Written for scope `dc` only, where an SSTable entry names its node instead of the manifest naming one node.
- `host_id`, `datacenter`, `rack` - as in the `node` member.

The `snapshot` member contains metadata about the snapshot.
- `name` - is the snapshot name (a.k.a. `tag`)
- `created_at` - is the time when the snapshot was created.
- `expires_at` - is an optional time when the snapshot expires and can be dropped, if a TTL was set for the snapshot.  If there is no TTL, `expires_at` may be omitted or set to 0.

The `table` member contains metadata about the table being snapshot.
- `keyspace_name` and `table_name` - are self-explanatory.
- `table_id` - a universally unique identifier (UUID) of the table set when the table is created.
- `tablets_type`:
    - `none` - if the keyspace uses vnodes replication
    - `powof2` - if the keyspace uses tablets replication, and the tablet token ranges are based on powers of 2.
    - `arbitrary` - if the keyspace uses tablets replication, and the tablet token ranges and count can be arbitrary.
- `tablet_count` - if `tablets_type` is not `none`, contains the number of tablets allocated in the table. If `tablets_type` is `powof2`, tablet_count would be a power of 2.

The `tablets` member is a list describing the tablets of the table at the time of the snapshot. Written for a table which uses tablets.
- `id` - is the tablet id.
- `first_token` and `last_token` - are the first and last tokens of the tablet's token range.
- `repair_time` and `repaired_at` - describe the last repair of the tablet. An SSTable whose `repaired_at` is not older than the `repaired_at` of its tablet belongs to the repaired set of that tablet, which is what lets cluster backup store one replica of the range only.

The `sstables` member is a list containing metadata about the SSTables in the snapshot.
- `id` - is the SSTable's unique id (a UUID).  It is carried over with the SSTable when it's streamed as part of tablet migration, even if it gets a new generation.
- `toc_name` - is the name of the SSTable Table Of Contents (TOC) component.
- `data_size` and `index_size` - are the sizes of the SSTable's data and index components, respectively.  They can be used to estimate how much disk space is needed for restore.
- `first_token` and `last_token` - are the first and last tokens in the SSTable, respectively.  They can be used to determine if a SSTable is fully contained in a (tablet) token range to enable efficient file-based streaming of the SSTable.
- `tablet_id` - is the id of the tablet which owned the SSTable. Written for a table which uses tablets.
- `repaired_at` - is the time of the repair the SSTable belongs to, or 0 for an SSTable which was never repaired.
- `node` - is the host_id of the node the SSTable was backed up from, described by the `nodes` member. Written for scope `dc` only.

### Restore into object-storage tables

Tablet-aware restore, the `/storage_service/tablets/restore` API, writes the downloaded components through the storage of the destination table, so a table which uses object storage receives them as objects of its own bucket.

A restore is a copy, so every restored SSTable receives a fresh `sstable_id` derived from its new generation, the same way a newly written SSTable does. Every replica which restores the same backup SSTable therefore receives an `sstable_id` of its own, and the component objects of the copies collide neither with each other nor with the backup, even when the destination keyspace uses the bucket which stores the backup.

A copy is created like any other new SSTable: a `system.sstables` entry with the `creating` status and a `refs/nodes/{host_id}/{generation}` reference object are created before the first component object is uploaded, and the status is changed to `sealed` once the SSTable is attached to the table. A restore which fails or is aborted therefore leaves behind entries whose status is not `sealed`, which boot time garbage collection removes together with their component objects on the next start of the node.

Which SSTables a node downloads is decided by the rows of `system_distributed.snapshot_sstables`, which are addressed by datacenter and rack. Restore inserts the rows of a manifest with scope `node` under the datacenter and the rack of the node the manifest describes. Cluster backup, which writes a manifest with scope `dc`, backs up the SSTables of a single node for a token range whose SSTables belong to the repaired set of its tablet, so the rack an SSTable was backed up from does not tell which replica of the destination table needs it. Restore therefore inserts the rows of a manifest with scope `dc` under every rack of the datacenter which may own a replica of the destination table.

## Downloading, deleting, uploading SSTables

To manually manage sstables on S3, AWS CLI commands can be used, but first it's mandatory to have awscli  
installed ([installation guide](https://docs.aws.amazon.com/cli/latest/userguide/getting-started-install.html)) and have the proper credentials set up in order to be able to access ScyllaDB S3 buckets.

Please make sure your `~/.aws/credentials` file points to a valid set of S3 credentials.  
Either refresh credentials if you use an OKTA-based fetching tool or make sure they point to a valid IAM user with S3 access.

Provided all the prerequisites above are fulfilled and you're able to run
```sh
aws s3 ls s3://your-bucket/
```
and see something (or at least not see an error if the bucket is empty), you're all set for the next commands.
> **NOTE:**
> Please refer to the sections above for the prefix layout of each S3 use case.

### Downloading SSTables

Fetching the SSTables of your backup can be easily done by  
e.g. copying each individual component
```sh
aws s3 cp s3://your-bucket/path/to/sstable/me-3gqb_1izi_0pxn421yzymfw5c8zf-big-Data.db  /local/path/to/sstable/component
```
or downloading an entire sstable using globs
```sh
aws s3 cp s3://your-bucket/path-to-sstables/ /local/path/for/sstables --exclude "*" --include 'some-sstable-generation-big-*' --recursive
```
### Deleting SSTables

components individually
```sh
aws s3 rm s3://your-bucket/path/to/sstable/me-3gqb_1izi_0pxn421yzymfw5c8zf-big-Data.db
```
or the entire SSTable using globs
```sh
aws s3 rm s3://your-bucket/path-to-sstables/ --exclude "*" --include 'some-sstable-generation-big-*' --recursive
```
### Uploading SSTables
components individually
```sh
aws s3 cp /local/path/to/sstable/me-3gqb_1izi_0pxn421yzymfw5c8zf-big-Data.db s3://your-bucket/path/to/sstable/component
```
or the entire SSTable using globs
```sh
aws s3 cp /local/path/for/sstables s3://your-bucket/path-to-sstables/ --exclude "*" --include 'some-sstable-generation-big-*' --recursive
```

## Metadata touchups

In case of Scylla Manager backups, if manual scrubbing is needed and SSTables will be re-uploaded,  
multiple things would need to be changed, same thing if you need to drop some SSTables altogether.  
As you might’ve seen in the Scylla Manager [Specification Docs](https://manager.docs.scylladb.com/stable/backup/specification.html), we keep a JSON manifest per node  
and that manifest file contains lots of SSTable-dependent information:

* list of SSTables per table owned by node
* total size of SSTables in the chunk of table owned
* total size of all chunks of tables owned
* the list of tokens owned by the node

As the name of the fields suggests, all the information in the list above depends on the SSTables content, so any attempt  
to fix locally a corrupt SSTable and re-upload, most probably will force you to update them in the manifest file of the node.  
There is high likelihood that a scrubbed SSTable results in different values for all the fields specified above.

For the `storage_service/backup` REST API, in theory only removing an entire SSTable from the backup would require changing  
the manifest file and remove the corresponding entry for the SSTable, in all other cases, no metadata changes needed.

For `CREATE KEYSPACE` on S3/GS storage, Scylla tracks object-storage SSTables in `system.sstables` and uses reference objects under the SSTable prefix to decide when component objects can be deleted. Manual changes to the objects under this layout should keep `system.sstables`, component objects, and `refs/nodes/{host_id}/{generation}` objects consistent.

> **NOTE:**
> It’s obvious to say that re-uploading a scrubbed SSTable means re-uploading all its components as it’s likely most of them were changed.
