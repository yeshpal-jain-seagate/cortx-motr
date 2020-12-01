======================================
High Level Design of Catalogue Service
======================================

This document presents a high level design (HLD) of the Motr catalogue service. The main purposes of this document are: (i) to be inspected by the Motr architects and peer designers to ascertain that high level design is aligned with Motr architecture and other designs, and contains no defects, (ii) to be a source of material for Active Reviews of Intermediate Design (ARID) and detailed level design (DLD) of the same component, (iii) to serve as a design reference document.

The intended audience of this document consists of Motr customers, architects, designers and developers.

*************
Introduction
*************

Catalogue service (cas) is a Motr service exporting key-value catalogues (indices). Users can access catalogues by sending appropriate fops to an instance of the catalogue service. Externally, a catalogue is a collection of key-value pairs, called records. A user can insert and delete records, lookup records by key and iterate through records in a certain order. A catalogue service does not interpret keys or values, (except that keys are ordered as bit-strings)—semantics are left to users.

Catalogues are used by other Motr sub-systems, to store and access meta-data. Distributed meta-data storage is implemented on top of cas.



