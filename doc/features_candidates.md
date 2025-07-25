# UBI Feature Candidates

- **Static UBI Volumes**

  Enables splitting a UBI physical partition into smaller logical volumes. This preserves UBI’s core advantages while allowing operations on smaller volumes.

- **Dynamic UBI Volumes**

  Unlike static volumes, which have fixed sizes, dynamic volumes can be resized easily. This flexibility is useful for devices in the field requiring partition size adjustments and migrations.

- **Hidden Write Block Alignment**

  Flash drivers require writes to be aligned to specific block sizes. Currently, applications must handle these constraints. This feature would move write alignment responsibility into UBI, simplifying application-level operations.

- **UBI Device Dual-Bank**

  Currently, UBI stores its metadata headers in a single physical eraseblock. Storing this metadata redundantly across two eraseblocks would enable safer updates and reduce the risk of corruption during updates.

- **Permanent Bad Blocks**

  UBI can track permanently unusable physical eraseblocks by storing this information in dedicated metadata headers, preventing their future use.

- **Allocation Tips**

  UBI can receive hints about allocation lifespan: short-term (e.g., logs) or long-term (e.g., certificates). This allows implementing free block scheduling policies that optimize flash usage and endurance.
