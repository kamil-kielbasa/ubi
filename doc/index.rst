UBI on Zephyr
=============

**Unsorted Block Images (UBI)** is a volume management layer for raw flash
devices running `Zephyr RTOS <https://www.zephyrproject.org/>`_.
It provides wear-leveling, bad block management, and multiple logical volumes
on a single flash partition — similar to what LVM does for block devices.

.. note::

   **Release status: v1.0.0.**
   The library API and the on-flash format (plain and secure) are
   stable; breaking changes require a major version bump. CI,
   coverage, and metrics badges live on the
   `repository README <https://github.com/kamil-kielbasa/ubi#readme>`_.
   See the `CHANGELOG <https://github.com/kamil-kielbasa/ubi/blob/main/CHANGELOG.md>`_
   for the full release history.

----

Where to start
--------------

Pick the card that matches what you came here to do.

.. list-table::
   :widths: 50 50
   :header-rows: 0

   * - **📖 New here?**

       Understand what UBI is and whether it fits your project.

       * :doc:`getting_started/what_is_ubi`
       * :doc:`getting_started/comparison`
       * :doc:`getting_started/concepts`
     - **🔧 Want to integrate?**

       Build, configure, and write your first volume.

       * :doc:`getting_started/quick_start`
       * :doc:`guide/configuration`
       * :doc:`guide/cookbook`

   * - **🏗 How does it work?**

       Read the design — plain UBI and Secure UBI explained.

       * :doc:`architecture/plain_architecture`
       * :doc:`architecture/secure_overview`
     - **📚 Looking up details?**

       Reference material for implementers and auditors.

       * :doc:`reference/api`
       * :doc:`reference/kconfig_reference`
       * :doc:`reference/glossary`
       * :doc:`reference/onflash_format_spec`
       * :doc:`reference/linux_ubi_comparison`

----

.. toctree::
   :maxdepth: 2
   :caption: Getting Started

   getting_started/what_is_ubi
   getting_started/comparison
   getting_started/quick_start
   getting_started/concepts

.. toctree::
   :maxdepth: 2
   :caption: User Guide

   guide/plain_workflow
   guide/secure_workflow
   guide/environment_setup
   guide/configuration
   guide/cookbook

.. toctree::
   :maxdepth: 2
   :caption: Architecture

   architecture/plain_architecture
   architecture/secure_overview

.. toctree::
   :maxdepth: 2
   :caption: Reference

   reference/api
   reference/kconfig_reference
   reference/error_codes
   reference/glossary
   reference/onflash_format_spec
   reference/linux_ubi_comparison

.. toctree::
   :maxdepth: 1
   :caption: Project

   project/roadmap
   project/contributing
   project/test_strategy
   project/changelog
