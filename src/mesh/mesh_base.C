// The libMesh Finite Element Library.
// Copyright (C) 2002-2014 Benjamin S. Kirk, John W. Peterson, Roy H. Stogner

// This library is free software; you can redistribute and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA



// library configuration
#include "libmesh/libmesh_config.h"

// C++ includes
#include <algorithm> // for std::min
#include <map>       // for std::multimap
#include <sstream>   // for std::ostringstream


// Local includes
#include "libmesh/boundary_info.h"
#include "libmesh/elem.h"
#include "libmesh/mesh_base.h"
#include "libmesh/parallel.h"
#include "libmesh/partitioner.h"
#include "libmesh/point_locator_base.h"
#include "libmesh/threads.h"

namespace libMesh
{



// ------------------------------------------------------------
// MeshBase class member functions
MeshBase::MeshBase (const Parallel::Communicator &comm_in,
                    unsigned char d) :
  ParallelObject (comm_in),
  boundary_info  (new BoundaryInfo(*this)),
  _n_parts       (1),
  _is_prepared   (false),
  _point_locator (),
  _partitioner   (),
#ifdef LIBMESH_ENABLE_UNIQUE_ID
  _next_unique_id(DofObject::invalid_unique_id),
#endif
  _skip_partitioning(libMesh::on_command_line("--skip-partitioning")),
  _skip_renumber_nodes_and_elements(false)
{
  _elem_dims.insert(d);
  libmesh_assert_less_equal (LIBMESH_DIM, 3);
  libmesh_assert_greater_equal (LIBMESH_DIM, d);
  libmesh_assert (libMesh::initialized());
}


#ifndef LIBMESH_DISABLE_COMMWORLD
MeshBase::MeshBase (unsigned char d) :
  ParallelObject (CommWorld),
  boundary_info  (new BoundaryInfo(*this)),
  _n_parts       (1),
  _is_prepared   (false),
  _point_locator (),
  _partitioner   (),
#ifdef LIBMESH_ENABLE_UNIQUE_ID
  _next_unique_id(DofObject::invalid_unique_id),
#endif
  _skip_partitioning(libMesh::on_command_line("--skip-partitioning")),
  _skip_renumber_nodes_and_elements(false)
{
  _elem_dims.insert(d);
  libmesh_assert_less_equal (LIBMESH_DIM, 3);
  libmesh_assert_greater_equal (LIBMESH_DIM, d);
  libmesh_assert (libMesh::initialized());
}
#endif // !LIBMESH_DISABLE_COMMWORLD



MeshBase::MeshBase (const MeshBase& other_mesh) :
  ParallelObject (other_mesh),
  boundary_info  (new BoundaryInfo(*this)),
  _n_parts       (other_mesh._n_parts),
  _is_prepared   (other_mesh._is_prepared),
  _point_locator (),
  _partitioner   (),
#ifdef LIBMESH_ENABLE_UNIQUE_ID
  _next_unique_id(other_mesh._next_unique_id),
#endif
  _skip_partitioning(libMesh::on_command_line("--skip-partitioning")),
  _skip_renumber_nodes_and_elements(false),
  _elem_dims(other_mesh._elem_dims)
{
  if(other_mesh._partitioner.get())
    {
      _partitioner = other_mesh._partitioner->clone();
    }
}



MeshBase::~MeshBase()
{
  this->clear();

  libmesh_exceptionless_assert (!libMesh::closed());
}

unsigned int MeshBase::mesh_dimension() const
{
  libmesh_assert(!_elem_dims.empty());
  return cast_int<unsigned int>(*_elem_dims.rbegin());
}

void MeshBase::prepare_for_use (const bool skip_renumber_nodes_and_elements, const bool skip_find_neighbors)
{
  parallel_object_only();

  libmesh_assert(this->comm().verify(this->is_serial()));

  // A distributed mesh may have processors with no elements (or
  // processors with no elements of higher dimension, if we ever
  // support mixed-dimension meshes), but we want consistent
  // mesh_dimension anyways.
  //
  // cache_elem_dims() should get the elem_dimensions() and
  // mesh_dimension() correct later, and we don't need it earlier.


  // Renumber the nodes and elements so that they in contiguous
  // blocks.  By default, _skip_renumber_nodes_and_elements is false.
  //
  // We may currently change that by passing
  // skip_renumber_nodes_and_elements==true to this function, but we
  // should use the allow_renumbering() accessor instead.
  //
  // Instances where you if prepare_for_use() should not renumber the nodes
  // and elements include reading in e.g. an xda/r or gmv file. In
  // this case, the ordering of the nodes may depend on an accompanying
  // solution, and the node ordering cannot be changed.

  if (skip_renumber_nodes_and_elements)
    {
      libmesh_deprecated();
      this->allow_renumbering(false);
    }

  // Mesh modification operations might not leave us with consistent
  // id counts, but our partitioner might need that consistency.
  if(!_skip_renumber_nodes_and_elements)
    this->renumber_nodes_and_elements();
  else
    this->update_parallel_id_counts();

  // Let all the elements find their neighbors
  if(!skip_find_neighbors)
    this->find_neighbors();

  // Partition the mesh.
  this->partition();

  // If we're using ParallelMesh, we'll want it parallelized.
  this->delete_remote_elements();

#ifdef LIBMESH_ENABLE_UNIQUE_ID
  // Assign DOF object unique ids
  this->assign_unique_ids();
#endif

  if(!_skip_renumber_nodes_and_elements)
    this->renumber_nodes_and_elements();

  // Search the mesh for all the dimensions of the elements
  // and cache them.
  this->cache_elem_dims();

  // Reset our PointLocator.  This needs to happen any time the elements
  // in the underlying elements in the mesh have changed, so we do it here.
  this->clear_point_locator();

  // The mesh is now prepared for use.
  _is_prepared = true;
}



void MeshBase::clear ()
{
  // Reset the number of partitions
  _n_parts = 1;

  // Reset the _is_prepared flag
  _is_prepared = false;

  // Clear boundary information
  this->get_boundary_info().clear();

  // Clear element dimensions
  _elem_dims.clear();

  // Clear our point locator.
  this->clear_point_locator();
}



void MeshBase::subdomain_ids (std::set<subdomain_id_type> &ids) const
{
  // This requires an inspection on every processor
  parallel_object_only();

  ids.clear();

  const_element_iterator el  = this->active_local_elements_begin();
  const_element_iterator end = this->active_local_elements_end();

  for (; el!=end; ++el)
    ids.insert((*el)->subdomain_id());

  // Some subdomains may only live on other processors
  this->comm().set_union(ids);
}



subdomain_id_type MeshBase::n_subdomains() const
{
  // This requires an inspection on every processor
  parallel_object_only();

  std::set<subdomain_id_type> ids;

  this->subdomain_ids (ids);

  return cast_int<subdomain_id_type>(ids.size());
}




dof_id_type MeshBase::n_nodes_on_proc (const processor_id_type proc_id) const
{
  // We're either counting a processor's nodes or unpartitioned
  // nodes
  libmesh_assert (proc_id < this->n_processors() ||
                  proc_id == DofObject::invalid_processor_id);

  return static_cast<dof_id_type>(std::distance (this->pid_nodes_begin(proc_id),
                                                 this->pid_nodes_end  (proc_id)));
}



dof_id_type MeshBase::n_elem_on_proc (const processor_id_type proc_id) const
{
  // We're either counting a processor's elements or unpartitioned
  // elements
  libmesh_assert (proc_id < this->n_processors() ||
                  proc_id == DofObject::invalid_processor_id);

  return static_cast<dof_id_type>(std::distance (this->pid_elements_begin(proc_id),
                                                 this->pid_elements_end  (proc_id)));
}



dof_id_type MeshBase::n_active_elem_on_proc (const processor_id_type proc_id) const
{
  libmesh_assert_less (proc_id, this->n_processors());
  return static_cast<dof_id_type>(std::distance (this->active_pid_elements_begin(proc_id),
                                                 this->active_pid_elements_end  (proc_id)));
}



dof_id_type MeshBase::n_sub_elem () const
{
  dof_id_type ne=0;

  const_element_iterator       el  = this->elements_begin();
  const const_element_iterator end = this->elements_end();

  for (; el!=end; ++el)
    ne += (*el)->n_sub_elem();

  return ne;
}



dof_id_type MeshBase::n_active_sub_elem () const
{
  dof_id_type ne=0;

  const_element_iterator       el  = this->active_elements_begin();
  const const_element_iterator end = this->active_elements_end();

  for (; el!=end; ++el)
    ne += (*el)->n_sub_elem();

  return ne;
}



std::string MeshBase::get_info() const
{
  std::ostringstream oss;

  oss << " Mesh Information:"                                  << '\n';

  if (!_elem_dims.empty())
    {
      oss << "  elem_dimensions()={";
      std::copy(_elem_dims.begin(),
                --_elem_dims.end(), // --end() is valid if the set is non-empty
                std::ostream_iterator<unsigned int>(oss, ", "));
      oss << cast_int<unsigned int>(*_elem_dims.rbegin());
      oss << "}\n";
    }

  oss << "  spatial_dimension()=" << this->spatial_dimension() << '\n'
      << "  n_nodes()="           << this->n_nodes()           << '\n'
      << "    n_local_nodes()="   << this->n_local_nodes()     << '\n'
      << "  n_elem()="            << this->n_elem()            << '\n'
      << "    n_local_elem()="    << this->n_local_elem()      << '\n'
#ifdef LIBMESH_ENABLE_AMR
      << "    n_active_elem()="   << this->n_active_elem()     << '\n'
#endif
      << "  n_subdomains()="      << static_cast<std::size_t>(this->n_subdomains()) << '\n'
      << "  n_partitions()="      << static_cast<std::size_t>(this->n_partitions()) << '\n'
      << "  n_processors()="      << static_cast<std::size_t>(this->n_processors()) << '\n'
      << "  n_threads()="         << static_cast<std::size_t>(libMesh::n_threads()) << '\n'
      << "  processor_id()="      << static_cast<std::size_t>(this->processor_id()) << '\n';

  return oss.str();
}


void MeshBase::print_info(std::ostream& os) const
{
  os << this->get_info()
     << std::endl;
}


std::ostream& operator << (std::ostream& os, const MeshBase& m)
{
  m.print_info(os);
  return os;
}


void MeshBase::partition (const unsigned int n_parts)
{
  // If we get here and we have unpartitioned elements, we need that
  // fixed.
  if (this->n_unpartitioned_elem() > 0)
    {
      libmesh_assert (partitioner().get());
      libmesh_assert (this->is_serial());
      partitioner()->partition (*this, n_parts);
    }
  // NULL partitioner means don't repartition
  // Non-serial meshes may not be ready for repartitioning here.
  else if(!skip_partitioning() &&
          partitioner().get() &&
          this->is_serial())
    {
      partitioner()->partition (*this, n_parts);
    }
  else
    {
      // Adaptive coarsening may have "orphaned" nodes on processors
      // whose elements no longer share them.  We need to check for
      // and possibly fix that.
      Partitioner::set_node_processor_ids(*this);

      // Make sure locally cached partition count
      this->recalculate_n_partitions();

      // Make sure any other locally cached data is correct
      this->update_post_partitioning();
    }
}

unsigned int MeshBase::recalculate_n_partitions()
{
  // This requires an inspection on every processor
  parallel_object_only();

  const_element_iterator el  = this->active_local_elements_begin();
  const_element_iterator end = this->active_local_elements_end();

  unsigned int max_proc_id=0;

  for (; el!=end; ++el)
    max_proc_id = std::max(max_proc_id, static_cast<unsigned int>((*el)->processor_id()));

  // The number of partitions is one more than the max processor ID.
  _n_parts = max_proc_id+1;

  this->comm().max(_n_parts);

  return _n_parts;
}



const PointLocatorBase& MeshBase::point_locator () const
{
  libmesh_deprecated();

  if (_point_locator.get() == NULL)
    {
      // PointLocator construction may not be safe within threads
      libmesh_assert(!Threads::in_threads);

      _point_locator.reset (PointLocatorBase::build(TREE_ELEMENTS, *this).release());
    }

  return *_point_locator;
}


UniquePtr<PointLocatorBase> MeshBase::sub_point_locator () const
{
  // If there's no master point locator, then we need one.
  if (_point_locator.get() == NULL)
    {
      // PointLocator construction may not be safe within threads
      libmesh_assert(!Threads::in_threads);

      // And it may require parallel communication
      parallel_object_only();

      _point_locator.reset (PointLocatorBase::build(TREE_ELEMENTS, *this).release());
    }

  // Otherwise there was a master point locator, and we can grab a
  // sub-locator easily.
  return PointLocatorBase::build(TREE_ELEMENTS, *this, _point_locator.get());
}



void MeshBase::clear_point_locator ()
{
  _point_locator.reset(NULL);
}



std::string& MeshBase::subdomain_name(subdomain_id_type id)
{
  return _block_id_to_name[id];
}

const std::string& MeshBase::subdomain_name(subdomain_id_type id) const
{
  // An empty string to return when no matching subdomain name is found
  static const std::string empty;

  std::map<subdomain_id_type, std::string>::const_iterator iter = _block_id_to_name.find(id);
  if (iter == _block_id_to_name.end())
    return empty;
  else
    return iter->second;
}




subdomain_id_type MeshBase::get_id_by_name(const std::string& name) const
{
  // Linear search over the map values.
  std::map<subdomain_id_type, std::string>::const_iterator
    iter = _block_id_to_name.begin(),
    end_iter = _block_id_to_name.end();

  for ( ; iter != end_iter; ++iter)
    if (iter->second == name)
      return iter->first;

  // If we made it here without returning, we don't have a subdomain
  // with the requested name, so return Elem::invalid_subdomain_id.
  return Elem::invalid_subdomain_id;
}

void MeshBase::cache_elem_dims()
{
  // This requires an inspection on every processor
  parallel_object_only();

  const_element_iterator el  = this->active_local_elements_begin();
  const_element_iterator end = this->active_local_elements_end();

  for (; el!=end; ++el)
    _elem_dims.insert((*el)->dim());

  // Some different dimension elements may only live on other processors
  this->comm().set_union(_elem_dims);
}

} // namespace libMesh
