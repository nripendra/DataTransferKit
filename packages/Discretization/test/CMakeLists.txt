# ##---------------------------------------------------------------------------##
# ## TESTS
# ##---------------------------------------------------------------------------##

TRIBITS_ADD_EXECUTABLE_AND_TEST(
  PointInCell
  SOURCES tstPointInCell.cpp unit_test_main.cpp
  COMM serial mpi
  NUM_MPI_PROCS 1
  STANDARD_PASS_OUTPUT
  FAIL_REGULAR_EXPRESSION "data race;leak;runtime error"
  )


TRIBITS_ADD_EXECUTABLE_AND_TEST(
  MeshGenerator
  SOURCES tstMeshGenerator.cpp unit_test_main.cpp
  COMM serial mpi
  NUM_MPI_PROCS 4
  STANDARD_PASS_OUTPUT
  FAIL_REGULAR_EXPRESSION "data race;leak;runtime error"
  )
TRIBITS_COPY_FILES_TO_BINARY_DIR(
  test
  SOURCE_FILES structured_2d.txt structured_3d.txt mixed_2d.txt mixed_3d.txt
  )
