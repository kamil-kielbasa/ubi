API Reference
=============

This reference is auto-generated from the Doxygen comments in ``ubi.h``.

Usage Notes
-----------

**Lifecycle**: A UBI device must be initialized with ``ubi_device_init()`` before any other
operations. After ``ubi_device_deinit()``, the handle is invalid.

**Thread safety**: All public functions acquire a per-device mutex. Multiple threads
may safely call UBI functions on the same device concurrently. **Do not call from ISR context.**

**Error model**: All functions return ``0`` on success or a negative ``errno`` code on failure.
Common error codes:

.. list-table::
   :widths: 15 85
   :header-rows: 1

   * - Code
     - Meaning
   * - ``-EINVAL``
     - NULL pointer or invalid parameter (bad vol_id, lnum out of range, invalid geometry).
   * - ``-ENOSPC``
     - No free PEBs available for the requested operation.
   * - ``-ENOENT``
     - Volume with given vol_id does not exist.
   * - ``-EEXIST``
     - A volume with the same name but different configuration already exists.
   * - ``-EROFS``
     - Device is in degraded read-only mode (reserved PEB redundancy lost).
   * - ``-EIO``
     - Flash I/O error (read, write, or erase failure).
   * - ``-ENOMEM``
     - Heap allocation failure (``k_malloc`` returned NULL).
   * - ``-ECANCELED``
     - Operation not applicable (e.g., resize on static volume, or leb_count unchanged).

**Typical API call sequence**::

    ubi_device_init()          -- initialize device
    ubi_volume_create()        -- create one or more volumes
    ubi_leb_write()            -- write data to a LEB
    ubi_leb_read()             -- read data from a LEB
    ubi_device_erase_peb()     -- reclaim dirty PEBs (call periodically)
    ubi_volume_remove()        -- remove volumes when no longer needed
    ubi_device_deinit()        -- shut down and free resources

Defines
-------

.. doxygendefine:: UBI_VOLUME_NAME_MAX_LEN

Data Structures
---------------

.. doxygenstruct:: ubi_mtd
   :members:

.. doxygenstruct:: ubi_device_info
   :members:

.. doxygenenum:: ubi_volume_type

.. doxygenstruct:: ubi_volume_config
   :members:

Device Management
-----------------

.. doxygenfunction:: ubi_device_init
.. doxygenfunction:: ubi_device_get_info
.. doxygenfunction:: ubi_device_erase_peb
.. doxygenfunction:: ubi_device_deinit

Volume Management
-----------------

.. doxygenfunction:: ubi_volume_create
.. doxygenfunction:: ubi_volume_resize
.. doxygenfunction:: ubi_volume_remove
.. doxygenfunction:: ubi_volume_get_info

LEB I/O
-------

.. doxygenfunction:: ubi_leb_write
.. doxygenfunction:: ubi_leb_read
.. doxygenfunction:: ubi_leb_map
.. doxygenfunction:: ubi_leb_unmap
.. doxygenfunction:: ubi_leb_is_mapped
.. doxygenfunction:: ubi_leb_get_size

Test API (CONFIG_UBI_TEST_API_ENABLE)
-------------------------------------

These functions are only available when ``CONFIG_UBI_TEST_API_ENABLE=y``.
Do not enable in production builds.

.. doxygenfunction:: ubi_device_check_invariants
.. doxygenfunction:: ubi_device_get_peb_ec
