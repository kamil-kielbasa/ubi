# UBI Feature Candidates

- **Flash Optimization**  
  The library is currently not optimized for flash usage, as the primary focus has been on delivering new functionality.  
  Future work should aim to reduce flash footprint and improve storage efficiency.

- **Full Device Dual-Banking**  
  UBI currently maintains metadata headers in two physical eraseblocks, but the dual-banking mechanism is only partially implemented. Extending this to full dual-bank support for both device and volume headers would improve reliability, enabling safer updates and reducing the risk of corruption during metadata operations.

- **Permanent Bad Block Management**  
  Today, UBI tracks bad blocks only in RAM. If additional blocks fail at runtime, they are handled until the next reboot, after which the information is lost.  
  Permanent bad block management would repeatedly stress-test suspicious blocks with multiple erase attempts. If the block consistently fails, it is marked as permanently bad and stored in non-volatile memory. This ensures the bad block record persists across reboots.

- **Thread Safety**  
  UBI requires a fair read–write locking mechanism that allows multiple concurrent readers or a single writer. The lock must guarantee fairness, ensuring writers eventually gain access without risk of starvation.

- **User-Space Tools**  
  Port the existing Linux UBI user-space utilities to Zephyr, providing developers with familiar tools for managing UBI devices and volumes.