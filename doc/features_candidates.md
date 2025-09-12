# UBI Feature Candidates

1) **Adopt Zephyr Flash Area API**
  Replace usage of low-level Zephyr flash APIs with the **Zephyr Flash Map (Flash Area API)**.  
  This change aligns the code with Zephyr's preferred practices and improves maintainability and integration with other Zephyr components.

2) **Functionality Refactoring**
    Comprehensive review and cleanup of existing UBI logic, including:
  
  - **Error Handling**  
    Verify all returned error codes for correctness and consistency.
  
  - **Logging**  
    Review and standardize all log messages.
  
  - **Write Retry Mechanism**  
    Introduce multiple write attempts in case of flash operation failures.
  
  - **Code Deduplication**  
    Identify and commonize duplicated logic throughout the codebase.
  
  - **File Structure Cleanup**  
    Move all direct flash operations from `ubi.c` to `ubi_utils.c` to separate logic from hardware interaction.
  
  - **Documentation**  
    Revisit and update all Doxygen comments to reflect the refactored logic and structure.

3) **Basic Thread Safety**  
  Add mutex lock/unlock to wrap each function call.

4) **Full Device Dual-Banking**  
  UBI currently maintains metadata headers in two physical eraseblocks, but the dual-banking mechanism is only partially implemented.  
  Extending this to full dual-bank support for both device and volume headers would improve reliability, enabling safer updates and reducing the risk of corruption during metadata operations.

5) **Permanent Bad Block Management**  
  Today, UBI tracks bad blocks only in RAM. If additional blocks fail at runtime, they are handled until the next reboot, after which the information is lost.  
  Permanent bad block management would repeatedly stress-test suspicious blocks with multiple erase attempts. If the block consistently fails, it is marked as permanently bad and stored in non-volatile memory.  
  This ensures the bad block record persists across reboots.

6) **Advanced Thread Safety**  
  UBI requires a fair read-write locking mechanism that allows multiple concurrent readers or a single writer.  
  The lock must guarantee fairness, ensuring writers eventually gain access without risk of starvation.

7) **Flash Optimization**  
  The library is currently not optimized for flash usage, as the primary focus has been on delivering new functionality.  
  Future work should aim to reduce flash footprint and improve storage efficiency.

8) **User-Space Tools**  
  Port the existing Linux UBI user-space utilities to Zephyr, providing developers with familiar tools for managing UBI devices and volumes.