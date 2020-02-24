/*
 MDAL - Mesh Data Abstraction Library (MIT License)
 Copyright (C) 2018 Lutra Consulting Ltd.
*/

#include <stddef.h>
#include <iosfwd>
#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cassert>
#include <limits>
#include <algorithm>

#include "mdal_2dm.hpp"
#include "mdal.h"
#include "mdal_utils.hpp"
#include "mdal_logger.hpp"

#define DRIVER_NAME "2DM"

MDAL::Mesh2dm::Mesh2dm( size_t verticesCount,
                        size_t edgesCount,
                        size_t facesCount,
                        size_t faceVerticesMaximumCount,
                        MDAL::BBox extent,
                        const std::string &uri,
                        const std::map<size_t, size_t> vertexIDtoIndex )
  : MemoryMesh( DRIVER_NAME,
                verticesCount,
                edgesCount,
                facesCount,
                faceVerticesMaximumCount,
                extent,
                uri )
  , mVertexIDtoIndex( vertexIDtoIndex )
{
}

MDAL::Mesh2dm::~Mesh2dm() = default;

bool _parse_vertex_id_gaps( std::map<size_t, size_t> &vertexIDtoIndex, size_t vertexIndex, size_t vertexID )
{
  if ( vertexIndex == vertexID )
    return false;

  std::map<size_t, size_t>::iterator search = vertexIDtoIndex.find( vertexID );
  if ( search != vertexIDtoIndex.end() )
  {
    MDAL::Log::warning( Warn_ElementNotUnique, DRIVER_NAME, "could not find vertex" );
    return true;
  }

  vertexIDtoIndex[vertexID] = vertexIndex;
  return false;
}

size_t MDAL::Mesh2dm::vertexIndex( size_t vertexID ) const
{
  auto ni2i = mVertexIDtoIndex.find( vertexID );
  if ( ni2i != mVertexIDtoIndex.end() )
  {
    return  ni2i->second; // convert from ID to index
  }
  return vertexID;
}

size_t MDAL::Mesh2dm::maximumVertexId() const
{
  size_t maxIndex = verticesCount() - 1;
  if ( mVertexIDtoIndex.empty() )
    return maxIndex;
  else
  {
    // std::map is sorted!
    size_t maxID = mVertexIDtoIndex.rbegin()->first;
    return std::max( maxIndex, maxID );
  }
}

MDAL::Driver2dm::Driver2dm():
  Driver( DRIVER_NAME,
          "2DM Mesh File",
          "*.2dm",
          Capability::ReadMesh | Capability::SaveMesh
        )
{
}

MDAL::Driver2dm *MDAL::Driver2dm::create()
{
  return new Driver2dm();
}

MDAL::Driver2dm::~Driver2dm() = default;

bool MDAL::Driver2dm::canReadMesh( const std::string &uri )
{
  std::ifstream in( uri, std::ifstream::in );
  std::string line;
  if ( !MDAL::getHeaderLine( in, line ) || !startsWith( line, "MESH2D" ) )
  {
    return false;
  }
  return true;
}

std::unique_ptr<MDAL::Mesh> MDAL::Driver2dm::load( const std::string &meshFile )
{
  mMeshFile = meshFile;

  MDAL::Log::resetLastStatus();

  std::ifstream in( mMeshFile, std::ifstream::in );
  std::string line;
  if ( !std::getline( in, line ) || !startsWith( line, "MESH2D" ) )
  {
    MDAL::Log::error( MDAL_Status::Err_UnknownFormat, name(), meshFile + " could not be opened" );
    return nullptr;
  }

  size_t faceCount = 0;
  size_t vertexCount = 0;
  size_t edgesCount = 0;

  // Find out how many nodes and elements are contained in the .2dm mesh file
  while ( std::getline( in, line ) )
  {
    if ( startsWith( line, "E4Q" ) ||
         startsWith( line, "E3T" ) )
    {
      faceCount++;
    }
    else if ( startsWith( line, "ND" ) )
    {
      vertexCount++;
    }
    else if ( startsWith( line, "E2L" ) )
    {
      edgesCount++;
    }
    else if ( startsWith( line, "E3L" ) ||
              startsWith( line, "E6T" ) ||
              startsWith( line, "E8Q" ) ||
              startsWith( line, "E9Q" ) )
    {
      MDAL::Log::warning( MDAL_Status::Err_UnsupportedElement, name(),  "found unsupported element" );
      return nullptr;
    }
  }

  // Allocate memory
  Vertices vertices( vertexCount );
  Edges edges( edgesCount );
  Faces faces( faceCount );

  // Basement 3.x supports definition of elevation for cell centers
  std::vector<double> elementCenteredElevation;

  in.clear();
  in.seekg( 0, std::ios::beg );

  std::vector<std::string> chunks;

  size_t faceIndex = 0;
  size_t vertexIndex = 0;
  size_t edgeIndex = 0;
  std::map<size_t, size_t> vertexIDtoIndex;
  size_t lastVertexID = 0;

  while ( std::getline( in, line ) )
  {
    if ( startsWith( line, "E4Q" ) ||
         startsWith( line, "E3T" )
       )
    {
      chunks = split( line,  ' ' );
      assert( faceIndex < faceCount );

      const size_t faceVertexCount = MDAL::toSizeT( line[1] );
      assert( ( faceVertexCount == 3 ) || ( faceVertexCount == 4 ) );

      Face &face = faces[faceIndex];
      face.resize( faceVertexCount );

      // chunks format here
      // E** id vertex_id1, vertex_id2, ... material_id (elevation - optional)
      // vertex ids are numbered from 1
      // Right now we just store node IDs here - we will convert them to node indices afterwards
      assert( chunks.size() > faceVertexCount + 1 );

      for ( size_t i = 0; i < faceVertexCount; ++i )
        face[i] = MDAL::toSizeT( chunks[i + 2] ) - 1; // 2dm is numbered from 1

      // OK, now find out if there is optional cell elevation (BASEMENT 3.x)
      if ( chunks.size() == faceVertexCount + 4 )
      {

        // initialize dataset if it is still empty
        if ( elementCenteredElevation.empty() )
        {
          elementCenteredElevation = std::vector<double>( faceCount, std::numeric_limits<double>::quiet_NaN() );
        }

        // add Bed Elevation (Face) value
        elementCenteredElevation[faceIndex] = MDAL::toDouble( chunks[ faceVertexCount + 3 ] );
      }

      faceIndex++;
    }
    else if ( startsWith( line, "E2L" ) )
    {
      // format: E2L id n1 n2 matid
      chunks = split( line,  ' ' );
      assert( edgeIndex < edgesCount );
      assert( chunks.size() > 4 );
      size_t startVertexIndex = MDAL::toSizeT( chunks[2] ) - 1; // 2dm is numbered from 1
      size_t endVertexIndex = MDAL::toSizeT( chunks[3] ) - 1; // 2dm is numbered from 1
      Edge &edge = edges[edgeIndex];
      edge.startVertex = startVertexIndex;
      edge.endVertex = endVertexIndex;
      edgeIndex++;
    }
    else if ( startsWith( line, "ND" ) )
    {
      chunks = split( line,  ' ' );
      size_t nodeID = toSizeT( chunks[1] );

      if ( nodeID != 0 )
      {
        // specification of 2DM states that ID should be positive integer numbered from 1
        // but it seems some formats do not respect that
        if ( ( lastVertexID != 0 ) && ( nodeID <= lastVertexID ) )
        {
          // the algorithm requires that the file has NDs orderer by index
          MDAL::Log::error( MDAL_Status::Err_InvalidData, name(), "nodes are not ordered by index" );
          return nullptr;
        }
        lastVertexID = nodeID;
      }
      nodeID -= 1; // 2dm is numbered from 1

      _parse_vertex_id_gaps( vertexIDtoIndex, vertexIndex, nodeID );
      assert( vertexIndex < vertexCount );
      Vertex &vertex = vertices[vertexIndex];
      vertex.x = toDouble( chunks[2] );
      vertex.y = toDouble( chunks[3] );
      vertex.z = toDouble( chunks[4] );
      vertexIndex++;
    }
  }

  for ( std::vector<Face>::iterator it = faces.begin(); it != faces.end(); ++it )
  {
    Face &face = *it;
    for ( Face::size_type nd = 0; nd < face.size(); ++nd )
    {
      size_t nodeID = face[nd];

      std::map<size_t, size_t>::iterator ni2i = vertexIDtoIndex.find( nodeID );
      if ( ni2i != vertexIDtoIndex.end() )
      {
        face[nd] = ni2i->second; // convert from ID to index
      }
      else if ( vertices.size() < nodeID )
      {
        MDAL::Log::warning( MDAL_Status::Warn_ElementWithInvalidNode, name(), "found invalid node" );
      }
    }
    //TODO check validity of the face
    //check that we have distinct nodes
  }

  std::unique_ptr< Mesh2dm > mesh(
    new Mesh2dm(
      vertices.size(),
      edges.size(),
      faces.size(),
      MAX_VERTICES_PER_FACE_2DM,
      computeExtent( vertices ),
      mMeshFile,
      vertexIDtoIndex
    )
  );
  mesh->faces = faces;
  mesh->vertices = vertices;
  mesh->edges = edges;

  // Add Bed Elevations
  MDAL::addFaceScalarDatasetGroup( mesh.get(), elementCenteredElevation, "Bed Elevation (Face)" );
  MDAL::addBedElevationDatasetGroup( mesh.get(), vertices );

  return std::unique_ptr<Mesh>( mesh.release() );
}

void MDAL::Driver2dm::save( const std::string &uri, MDAL::Mesh *mesh )
{
  MDAL::Log::resetLastStatus();

  std::ofstream file( uri, std::ofstream::out );

  if ( !file.is_open() )
  {
    MDAL::Log::error( MDAL_Status::Err_FailToWriteToDisk, name(), "Could not open file " + uri );
  }

  std::string line = "MESH2D";
  file << line << std::endl;

  // write vertices
  std::unique_ptr<MDAL::MeshVertexIterator> vertexIterator = mesh->readVertices();
  double vertex[3];
  for ( size_t i = 0; i < mesh->verticesCount(); ++i )
  {
    vertexIterator->next( 1, vertex );
    line = "ND ";
    line.append( std::to_string( i + 1 ) );
    for ( size_t j = 0; j < 2; ++j )
    {
      line.append( " " );
      line.append( MDAL::coordinateToString( vertex[j] ) );
    }
    line.append( " " );
    line.append( MDAL::doubleToString( vertex[2] ) );

    file << line << std::endl;
  }

  // write faces
  std::unique_ptr<MDAL::MeshFaceIterator> faceIterator = mesh->readFaces();
  for ( size_t i = 0; i < mesh->facesCount(); ++i )
  {
    int faceOffsets[1];
    int vertexIndices[MAX_VERTICES_PER_FACE_2DM];
    faceIterator->next( 1, faceOffsets, 4, vertexIndices );

    if ( faceOffsets[0] > 2 && faceOffsets[0] < 5 )
    {
      if ( faceOffsets[0] == 3 )
        line = "E3T ";
      if ( faceOffsets[0] == 4 )
        line = "E4Q ";

      line.append( std::to_string( i + 1 ) );

      for ( int j = 0; j < faceOffsets[0]; ++j )
      {
        line.append( " " );
        line.append( std::to_string( vertexIndices[j] + 1 ) );
      }
    }
    file << line << std::endl;
  }

  // write edges
  std::unique_ptr<MDAL::MeshEdgeIterator> edgeIterator = mesh->readEdges();
  for ( size_t i = 0; i < mesh->edgesCount(); ++i )
  {
    int startIndex;
    int endIndex;
    edgeIterator->next( 1, &startIndex, &endIndex );
    line = "E2L " + std::to_string( mesh->facesCount() + i + 1 ) + " " + std::to_string( startIndex + 1 ) + " " + std::to_string( endIndex + 1 ) + " 1";
    file << line << std::endl;
  }

  file.close();
}
