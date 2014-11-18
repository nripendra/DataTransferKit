//---------------------------------------------------------------------------//
/*
  Copyright (c) 2014, Stuart R. Slattery
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  *: Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  *: Redistributions in binary form must reproduce the above copyright
  notice, this list of conditions and the following disclaimer in the
  documentation and/or other materials provided with the distribution.

  *: Neither the name of the Oak Ridge National Laboratory nor the
  names of its contributors may be used to endorse or promote products
  derived from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/
//---------------------------------------------------------------------------//
/*!
 * \file   consistent_interpolation.cpp
 * \author Stuart Slattery
 * \brief  STK file-based consistent interpolation example.
 */
//---------------------------------------------------------------------------//

#include <iostream>
#include <vector>
#include <cmath>
#include <sstream>
#include <algorithm>
#include <cassert>
#include <ctime>
#include <cstdlib>

#include "DTK_STKMeshEntitySet.hpp"
#include "DTK_STKMeshEntityLocalMap.hpp"
#include "DTK_STKMeshNodalShapeFunction.hpp"
#include "DTK_STKMeshDOFVector.hpp"
#include "DTK_STKMeshEntityPredicates.hpp"
#include "DTK_ConsistentInterpolationOperator.hpp"

#include <Teuchos_GlobalMPISession.hpp>
#include "Teuchos_CommandLineProcessor.hpp"
#include "Teuchos_XMLParameterListCoreHelpers.hpp"
#include "Teuchos_ParameterList.hpp"
#include <Teuchos_DefaultComm.hpp>
#include <Teuchos_DefaultMpiComm.hpp>
#include <Teuchos_CommHelpers.hpp>
#include <Teuchos_RCP.hpp>
#include <Teuchos_ArrayRCP.hpp>
#include <Teuchos_Array.hpp>
#include <Teuchos_OpaqueWrapper.hpp>
#include <Teuchos_TypeTraits.hpp>
#include <Teuchos_VerboseObject.hpp>
#include <Teuchos_StandardCatchMacros.hpp>
#include <Teuchos_TimeMonitor.hpp>

#include <Tpetra_MultiVector.hpp>

#include <stk_util/parallel/Parallel.hpp>

#include <stk_mesh/base/MetaData.hpp>
#include <stk_mesh/base/BulkData.hpp>
#include <stk_mesh/base/Types.hpp>
#include <stk_mesh/base/FieldBase.hpp>
#include <stk_mesh/base/Field.hpp>
#include <stk_mesh/base/GetEntities.hpp>
#include <stk_mesh/base/Selector.hpp>
#include <stk_topology/topology.hpp>

#include <stk_io/IossBridge.hpp>
#include <stk_io/StkMeshIoBroker.hpp>

#include <init/Ionit_Initializer.h>
#include <Ioss_SubSystem.h>

//---------------------------------------------------------------------------//
// Example driver.
//---------------------------------------------------------------------------//
int main(int argc, char* argv[])
{
    // INITIALIZATION
    // --------------

    // Setup communication.
    Teuchos::GlobalMPISession mpiSession(&argc,&argv);

    Teuchos::RCP<const Teuchos::Comm<int> > comm = 
	Teuchos::DefaultComm<int>::getComm();

    // Read in command line options.
    std::string xml_input_filename;
    Teuchos::CommandLineProcessor clp(false);
    clp.setOption( "xml-in-file",
		   &xml_input_filename,
		   "The XML file to read into a parameter list" );
    clp.parse(argc,argv);

    // Build the parameter list from the xml input.
    Teuchos::RCP<Teuchos::ParameterList> plist =
	Teuchos::rcp( new Teuchos::ParameterList() );
    Teuchos::updateParametersFromXmlFile(
	xml_input_filename, Teuchos::inoutArg(*plist) );

    // Read command-line options
    std::string source_mesh_input_file = plist->get<std::string>("Source Mesh Input File");
    std::string source_mesh_output_file = plist->get<std::string>("Source Mesh Output File");
    std::string source_mesh_part_name = plist->get<std::string>("Source Mesh Part");
    std::string target_mesh_input_file = plist->get<std::string>("Target Mesh Input File");
    std::string target_mesh_output_file = plist->get<std::string>("Target Mesh Output File");
    std::string target_mesh_part_name = plist->get<std::string>("Target Mesh Part");

    // Get the raw mpi communicator (basic typedef in STK).
    Teuchos::RCP<const Teuchos::MpiComm<int> > mpi_comm = 
	Teuchos::rcp_dynamic_cast<const Teuchos::MpiComm<int> >( comm );
    Teuchos::RCP<const Teuchos::OpaqueWrapper<MPI_Comm> > opaque_comm = 
	mpi_comm->getRawMpiComm();
    stk::ParallelMachine parallel_machine = (*opaque_comm)();

    
    // SOURCE MESH READ
    // ----------------

    // Load the source mesh.
    stk::io::StkMeshIoBroker src_broker( parallel_machine );
    std::size_t src_input_index = src_broker.add_mesh_database(
	source_mesh_input_file, "exodus", stk::io::READ_MESH );
    src_broker.set_active_mesh( src_input_index );
    src_broker.create_input_mesh();

    // Add a nodal field to the source part.
    stk::mesh::Field<double>& source_field = 
	src_broker.meta_data().declare_field<stk::mesh::Field<double> >( 
	    stk::topology::NODE_RANK, "u_src" );
    stk::mesh::Part* src_part = src_broker.meta_data().get_part( source_mesh_part_name );
    stk::mesh::put_field( source_field, *src_part );

    // Create the source bulk data.
    src_broker.populate_bulk_data();
    Teuchos::RCP<stk::mesh::BulkData> src_bulk_data = 
	Teuchos::rcpFromRef( src_broker.bulk_data() );

    // Put some data in the source field. We will use the node ids as the
    // scalar data.
    stk::mesh::Selector src_stk_selector( *src_part );
    stk::mesh::BucketVector src_part_buckets = 
	src_stk_selector.get_buckets( stk::topology::NODE_RANK );
    std::vector<stk::mesh::Entity> src_part_nodes;
    stk::mesh::get_selected_entities( 
	src_stk_selector, src_part_buckets, src_part_nodes );
    double* src_field_data;
    for ( stk::mesh::Entity entity : src_part_nodes )
    {
	src_field_data = stk::mesh::field_data( source_field, entity );
	src_field_data[0] = 2.0 * src_bulk_data->identifier( entity );
    }


    // TARGET MESH READ
    // ----------------

    // Load the target mesh.
    stk::io::StkMeshIoBroker tgt_broker( parallel_machine );
    std::size_t tgt_input_index = tgt_broker.add_mesh_database(
    	target_mesh_input_file, "exodus", stk::io::READ_MESH );
    tgt_broker.set_active_mesh( tgt_input_index );
    tgt_broker.create_input_mesh();

    // Add a nodal field to the target part.
    stk::mesh::Field<double>& target_field = 
    	tgt_broker.meta_data().declare_field<stk::mesh::Field<double> >( 
    	    stk::topology::NODE_RANK, "u_tgt" );
    stk::mesh::Part* tgt_part = tgt_broker.meta_data().get_part( target_mesh_part_name );
    stk::mesh::put_field( target_field, *tgt_part );

    // Create the target bulk data.
    tgt_broker.populate_bulk_data();
    Teuchos::RCP<stk::mesh::BulkData> tgt_bulk_data = 
    	Teuchos::rcpFromRef( tgt_broker.bulk_data() );

    
    // SOLUTION TRANSFER SETUP
    // -----------------------
    
    // Create a selector for the source part elements.
    DataTransferKit::STKSelectorPredicate src_predicate( src_stk_selector );
    Teuchos::RCP<DataTransferKit::EntitySelector> src_entity_selector =
    	Teuchos::rcp( new DataTransferKit::EntitySelector(
    			  DataTransferKit::ENTITY_TYPE_VOLUME, src_predicate.getFunction() ) );

    // Create a selector for the target part nodes.
    stk::mesh::Selector tgt_stk_selector( *tgt_part );
    DataTransferKit::STKSelectorPredicate tgt_predicate( tgt_stk_selector );
    Teuchos::RCP<DataTransferKit::EntitySelector> tgt_entity_selector =
    	Teuchos::rcp( new DataTransferKit::EntitySelector(
    			  DataTransferKit::ENTITY_TYPE_NODE, tgt_predicate.getFunction() ) );

    // Create a function space for the source.
    Teuchos::RCP<DataTransferKit::EntitySet> src_entity_set =
    	Teuchos::rcp( new DataTransferKit::STKMeshEntitySet(src_bulk_data) );
    Teuchos::RCP<DataTransferKit::EntityLocalMap> src_local_map =
    	Teuchos::rcp( new DataTransferKit::STKMeshEntityLocalMap(src_bulk_data) );
    Teuchos::RCP<DataTransferKit::STKMeshNodalShapeFunction> src_shape_function =
    	Teuchos::rcp( new DataTransferKit::STKMeshNodalShapeFunction(src_bulk_data) );
    Teuchos::RCP<DataTransferKit::FunctionSpace> src_function_space =
    	Teuchos::rcp( new DataTransferKit::FunctionSpace( 
    			  src_entity_set, src_local_map, src_shape_function ) );

    // Create a function space for the target.
    Teuchos::RCP<DataTransferKit::EntitySet> tgt_entity_set =
    	Teuchos::rcp( new DataTransferKit::STKMeshEntitySet(tgt_bulk_data) );
    Teuchos::RCP<DataTransferKit::EntityLocalMap> tgt_local_map =
    	Teuchos::rcp( new DataTransferKit::STKMeshEntityLocalMap(tgt_bulk_data) );
    Teuchos::RCP<DataTransferKit::STKMeshNodalShapeFunction> tgt_shape_function =
    	Teuchos::rcp( new DataTransferKit::STKMeshNodalShapeFunction(tgt_bulk_data) );
    Teuchos::RCP<DataTransferKit::FunctionSpace> tgt_function_space =
    	Teuchos::rcp( new DataTransferKit::FunctionSpace( 
    			  tgt_entity_set, tgt_local_map, tgt_shape_function ) );

    // Create a solution vector for the source.
    Teuchos::RCP<Tpetra::MultiVector<double,int,std::size_t> > src_vector =
    	DataTransferKit::STKMeshDOFVector::pullTpetraMultiVectorFromSTKField<double>(
    	    *src_bulk_data, source_field, 1 );	
    
    // Create a solution vector for the target.
    Teuchos::RCP<Tpetra::MultiVector<double,int,std::size_t> > tgt_vector =
    	DataTransferKit::STKMeshDOFVector::pullTpetraMultiVectorFromSTKField<double>(
    	    *tgt_bulk_data, target_field, 1 );


    // SOLUTION TRANSFER
    // -----------------

    // Solution transfer parameters.
    Teuchos::RCP<Teuchos::ParameterList> parameters = Teuchos::parameterList();

    // Create a consistent interpolation operator.
    Teuchos::RCP<DataTransferKit::MapOperator<double> > interpolation_operator =
    	Teuchos::rcp(
    	    new DataTransferKit::ConsistentInterpolationOperator<double>(
    		comm, src_entity_selector, tgt_entity_selector ) );

    // Setup the consistent interpolation operator.
    interpolation_operator->setup( src_vector->getMap(),
    				   src_function_space,
    				   tgt_vector->getMap(),
    				   tgt_function_space,
    				   parameters );

    // Apply the consistent interpolation operator.
    interpolation_operator->apply( *src_vector, *tgt_vector );

    // Push the target vector onto the target mesh.
    DataTransferKit::STKMeshDOFVector::pushTpetraMultiVectorToSTKField(
    	*tgt_vector, *tgt_bulk_data, target_field );

    // SOURCE MESH WRITE
    // -----------------

    std::size_t src_output_index = src_broker.create_output_mesh(
	source_mesh_output_file, stk::io::WRITE_RESULTS );
    src_broker.add_field( src_output_index, source_field );
    src_broker.begin_output_step( src_output_index, 0.0 );
    src_broker.write_defined_output_fields( src_output_index );
    src_broker.end_output_step( src_output_index );


    // TARGET MESH WRITE
    // -----------------

    std::size_t tgt_output_index = tgt_broker.create_output_mesh(
    	target_mesh_output_file, stk::io::WRITE_RESULTS );
    tgt_broker.add_field( tgt_output_index, target_field );
    tgt_broker.begin_output_step( tgt_output_index, 0.0 );
    tgt_broker.write_defined_output_fields( tgt_output_index );
    tgt_broker.end_output_step( tgt_output_index );
}

//---------------------------------------------------------------------------//
// end tstSTK_Mesh.cpp
//---------------------------------------------------------------------------//
