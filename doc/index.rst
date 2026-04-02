UBI on Zephyr
=============

**Unsorted Block Images (UBI)** is a volume management layer for raw flash
devices running `Zephyr RTOS <https://www.zephyrproject.org/>`_.
It provides wear-leveling, bad block management, and multiple logical volumes
on a single flash partition — similar to what LVM does for block devices.

This is a from-scratch implementation targeting resource-constrained embedded
systems. On `b_u585i_iot02a` (STM32U5 Cortex-M33), the **sample** application
build reports about **8.5 KB of flash** for `lib..__ubi__lib.a` and **24 bytes**
of static RAM in that library (partition guard). See :doc:`introduction` for
methodology and test-build figures.

Start Here
----------

New to UBI? Read these pages first:

1. **Overview** — key concepts (PEB, LEB, EC, VID) and how UBI works in 6 steps.
2. **Introduction** — why UBI exists, what it provides, and resource usage.
3. **Getting Started** — build, run tests, and evaluate on the simulator in minutes.

.. toctree::
   :maxdepth: 2
   :caption: Understanding UBI

   overview
   introduction
   architecture

.. toctree::
   :maxdepth: 2
   :caption: Using UBI

   getting_started
   configuration
   api

.. toctree::
   :maxdepth: 1
   :caption: Quality and Testing

   test_strategy

.. toctree::
   :maxdepth: 1
   :caption: Project

   roadmap
   design_proposal_crypto
   contributing
   changelog
