UBI on Zephyr
=============

**Unsorted Block Images (UBI)** is a volume management layer for raw flash
devices running `Zephyr RTOS <https://www.zephyrproject.org/>`_.
It provides wear-leveling, bad block management, and multiple logical volumes
on a single flash partition — similar to what LVM does for block devices.

.. note::

   **Release status: v1.0.0 (in preparation).**
   The library API and on-flash format are stabilising for v1.0.0. CI,
   coverage, and metrics badges live on the
   `repository README <https://github.com/kamil-kielbasa/ubi#readme>`_.

----

Where to start
--------------

Pick the card that matches what you came here to do.

.. list-table::
   :widths: 50 50
   :header-rows: 0

   * - **📖 New here?**

       Understand what UBI is and whether it fits your project.

       * :doc:`what_is_ubi`
       * :doc:`concepts`
     - **🔧 Want to integrate?**

       Build, configure, and write your first volume.

       * :doc:`quick_start`
       * :doc:`configuration`
       * :doc:`cookbook`

   * - **🏗 How does it work?**

       Read the design — plain UBI and Secure UBI explained.

       * :doc:`plain_architecture`
       * :doc:`secure_overview`
     - **📚 Looking up details?**

       Reference material for implementers and auditors.

       * :doc:`api`
       * :doc:`kconfig_reference`
       * :doc:`onflash_format_spec`

----

.. toctree::
   :maxdepth: 2
   :caption: Getting Started

   what_is_ubi
   quick_start
   concepts

.. toctree::
   :maxdepth: 2
   :caption: User Guide

   plain_workflow
   secure_workflow
   configuration
   cookbook

.. toctree::
   :maxdepth: 2
   :caption: Architecture

   plain_architecture
   secure_overview

.. toctree::
   :maxdepth: 2
   :caption: Reference

   api
   kconfig_reference
   error_codes
   onflash_format_spec

.. toctree::
   :maxdepth: 1
   :caption: Project

   roadmap
   contributing
   test_strategy
   changelog

.. Legacy pages (overview, introduction, getting_started, secure_architecture,
   secure_volume_lifecycle, secure_recovery_notes, secure_runtime_policy) are
   marked ``orphan: true`` while their content is being merged into the new
   sections (PRs 2–4). They keep their URLs but no longer appear in any toctree.
