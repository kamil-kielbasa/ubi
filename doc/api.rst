API Reference
=============

This reference is auto-generated from the Doxygen comments in ``ubi.h``.

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
