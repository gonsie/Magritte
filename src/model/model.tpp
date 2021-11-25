#include <cmath>
#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/Sparse>
#include <cfloat>
#include <set>
#include "tools/types.hpp"
#include <limits>
#include <algorithm>
#include "voro++.hh"
#include <memory>
#include <nanoflann.hpp>

///  Calculates the distance squared between two points
///    @param[in] point1: The index of the first point
///    @param[in] point2: The inddx of the second point
/////////////////////////////////////////////////////////
inline double Model::calc_distance2(Size point1,Size point2)
{
    return (geometry.points.position[point1]-geometry.points.position[point2]).squaredNorm();
}

///  Calculates the relative difference of the density of point1 with respect to point2
///    @param[in] point1: The index of the first point
///    @param[in] point2: The index of the second point
///    @returns: abs((rho1-rho2)/(rho1+rho2))
///////////////////////////////////////////////////////////////////////////////////////
inline double Model :: calc_diff_abundance_with_point(Size point1, Size point2)
{
    double abundance_self=chemistry.species.abundance[point1][1];
    double abundance_other=chemistry.species.abundance[point2][1];
    double temp_diff=std::abs((abundance_self-abundance_other)/(abundance_self+abundance_other));
    return temp_diff;
}


///  Returns whether two points are similar enough, according to the tolerance
///    @param[in] point1: The first point to check
///    @param[in] point2: The second point to check
///    @param[in] tolerance: The tolerance; Should lie between 0 (no coarsening) and 1 (everything may be coarsened)
/////////////////////////////////////////////////////////////////////////////
inline bool Model::points_are_similar(Size point1, Size point2, double tolerance)
{
    return (calc_diff_abundance_with_point(point1,point2)<tolerance);
}


///  Coarsen the mesh, i.e. add another coarser level
///    @param[in] tol: The coarsening tolerance used for coarsening the grid
///    @param[in] new_coars_lvl: The new coarsening level
////////////////////////////////////////////////////////////////////////////
inline void Model::coarsen(double tol, Size new_coars_lvl)
{
    // Adding a new grid; thus introducing its mask
    geometry.points.multiscale.mask[new_coars_lvl].resize(parameters.npoints());

    // Copying the values for the previous grid
    for (Size index=0; index<parameters.npoints(); index++)
    {
        geometry.points.multiscale.mask[new_coars_lvl][index]=geometry.points.multiscale.mask[new_coars_lvl-1][index];
    }

    geometry.points.multiscale.neighbors.push_back(vector<std::set<Size>>(geometry.points.multiscale.neighbors.back()));//should be deep copy
    std::set<Size> points_coarsened_around;
    geometry.points.multiscale.set_curr_coars_lvl(new_coars_lvl);
    std::cout<<"curr coarsening level: "<<geometry.points.multiscale.curr_coarsening_lvl<<std::endl;

    vector<Size> points_to_process=geometry.points.multiscale.get_current_points_in_grid();
    for (Size p: points_to_process)
    {
        if (can_be_coarsened(p, points_coarsened_around, tol, new_coars_lvl))
        {
            coarsen_around_point(p, new_coars_lvl);
            points_coarsened_around.insert(p);
        }
    }
}


///  Returns whether the mesh around a point can be coarsened
///    @param[in] p: The index of the point
///    @param[in] points_coarsened_around:  The set of point that are already coarsened around in this coarsening step
///    @param[in] tol: The coarsening tolerance
/////////////////////////////////////////////////////////////
inline bool Model::can_be_coarsened (const Size p, std::set<Size>& points_coarsened_around, double tol, Size new_coars_lvl)
{
    if (!geometry.points.multiscale.mask[new_coars_lvl][p]||!geometry.not_on_boundary(p))
    {
        return false;
    }//if point no longer in grid, do not coarsen
    //if the point lies on the boundary, do not waste time trying to coarsen around it

    for (const Size n : geometry.points.multiscale.neighbors.back()[p])
    {
        // Do not coarsen if a neighbor was already coarsend at this level,
        // this avoids creating large holes in the mesh.
        if (points_coarsened_around.find(n)!=points_coarsened_around.end()) {
            return false;}

        // Do not coarsen if the required coarsening criterion does not hold.
        if(!points_are_similar(p,n,tol)) {
            return false;}
    }

    // Otherwise coarsen.
    return true;
}

///  Coarsens the mesh around a point p
///    @param[in] p: The index of the point to coarsen around
/////////////////////////////////////////////////////////////
///  Will delete all (non-neighbor) points around p and then reconnect all neighbors of neighbors
///  such that we have a delaunay grid
inline void Model::coarsen_around_point (const Size p, Size new_coars_lvl)
{
    // Boundary neighbors need to be treated differently
    std::set<Size> boundary_neighbors;

    // Delete all neighbors around the current point (p),
    // i.e. remove its neighbors from the mesh by masking
    // TODO: figure out whether it is ok/necessary to empty their neighbors
    //TODO: get the points to remove from somewhere else
    for (const Size n : geometry.points.multiscale.neighbors.back()[p])
    {
        if (geometry.not_on_boundary(n))//boundary points will NEVER get removed
        {
            geometry.points.multiscale.mask[new_coars_lvl][n] = false;
            //maps neighbor to point which deleted it (fallback plan for interpolation)
            geometry.points.multiscale.point_deleted_map.insert(std::pair<Size,Size>(n,p));
        }
    }

    // Contains the neighbors of neighbors
    // Also can contain non-deleted neighbors
    std::set<Size> neighbors_of_neighbors;

    for (const Size n : geometry.points.multiscale.neighbors.back()[p])
    {
        if (geometry.not_on_boundary(n))
        {
            for (const Size n_n : geometry.points.multiscale.neighbors.back()[n])
            {
                if (geometry.points.multiscale.mask[new_coars_lvl][n_n]&&n_n!=p)//if neighbor of neighbor is still in the grid, i.e. not just a neighbor; also, do never try to add a point as its own neighbor!
                {
                    neighbors_of_neighbors.insert(n_n);// add neighbor of neighbor to the vector
                    geometry.points.multiscale.neighbors.back()[n_n].erase(n);//remove n from neighbors of n_n
                }
            }
            geometry.points.multiscale.neighbors.back()[n]=std::set<Size>();//and finally also delete every neighbor of the deleted point
        }
        else
        {//also do not forget to actually add our boundary elements as a neighbor of our point
            boundary_neighbors.insert(n);
            neighbors_of_neighbors.insert(n);
        }
    }

    //for now just the neighbors of neighbors of neighbors
    std::set<Size> container_points;
    for (const Size aff_point: neighbors_of_neighbors)
    {
        for (const Size n_n_n : geometry.points.multiscale.neighbors.back()[aff_point])
        {
            if (geometry.points.multiscale.mask[new_coars_lvl][n_n_n]&&n_n_n!=p)//add if neighbor of affected point is still in the grid, i.e. not just a neighbor; also, do never try to add a point as its own neighbor!!!!
            {
                container_points.insert(n_n_n);
            }
        }
    }

    //now also contains neighbors of neighbors and their neighbors
    container_points.insert(neighbors_of_neighbors.begin(),neighbors_of_neighbors.end());

    //clear the old neighbors of point p
    geometry.points.multiscale.neighbors.back()[p]=std::set<Size>();
    //Also clear point p from all boundary neighbors. Otherwise edge cases might occur that result in the neighbors not being symmetric.
    //  These neighbors get recalculated anyway
    for (Size bound_neigh:boundary_neighbors)
    {
        geometry.points.multiscale.neighbors.back()[bound_neigh].erase(p);
    }


    //contains the non-deleted neighbors and the neighbors of deleted neighbors
    std::vector<Size> affected_points;
    affected_points.reserve(1+neighbors_of_neighbors.size());
    affected_points.push_back(p);
    std::copy(container_points.begin(), container_points.end(), std::back_inserter(affected_points));

    //correctly setting up a rectangular boundary for all affected points
    double x_min=std::numeric_limits<double>::max();
    double x_max=std::numeric_limits<double>::min();
    double y_min=std::numeric_limits<double>::max();
    double y_max=std::numeric_limits<double>::min();
    double z_min=std::numeric_limits<double>::max();
    double z_max=std::numeric_limits<double>::min();

    for (Size aff_point: affected_points)
    {
        Vector3D pos=geometry.points.position[aff_point];
        if (x_min>pos.x())
        {x_min=pos.x();}
        if (x_max<pos.x())
        {x_max=pos.x();}
        if (y_min>pos.y())
        {y_min=pos.y();}
        if (y_max<pos.y())
        {y_max=pos.y();}
        if (z_min>pos.z())
        {z_min=pos.z();}
        if (z_max<pos.z())
        {z_max=pos.z();}
    }
    //also making sure that all points lie strictly inside the boundary
    double deltax=x_max-x_min;
    double deltay=y_max-y_min;
    double deltaz=z_max-z_min;
    //TODO/FIXME: also make sure that no overflow can ever happen!!
    //+-1 is a fix for also allowing 1D, 2D simulations
    x_max=x_max+0.001*deltax+1.0;
    x_min=x_min-0.001*deltax-1.0;
    y_max=y_max+0.001*deltay+1.0;
    y_min=y_min-0.001*deltay-1.0;
    z_max=z_max+0.001*deltaz+1.0;
    z_min=z_min-0.001*deltaz-1.0;

    voro::container con(x_min,x_max,y_min,y_max,z_min,z_max,8,8,8,
                  			false,false,false,8);

    voro::particle_order p_order;
    voro::voronoicell_neighbor cell;

  	// add particles into the container, starting with point p
  	for(Size aff_point:affected_points)
    {
        Vector3D pos=geometry.points.position[aff_point];
        con.put(p_order,static_cast<int>(aff_point),pos.x(),pos.y(),pos.z());
    }

    voro::c_loop_order l_order(con,p_order);

    //back to the beginning
    l_order.start();
    //for point p, just add all found neighbors to its neighbors list
    con.compute_cell(cell,l_order);
    std::vector<int> found_neighbors;
    cell.neighbors(found_neighbors);
    for (int new_neighbor:found_neighbors)
    {
        //note: voro++ returns negative values for the boundaries//not that it should play a role here
        if (new_neighbor>=0)
        { //and finally adding new neighbors to point p
            geometry.points.multiscale.neighbors.back()[p].insert(static_cast<Size>(new_neighbor));
            geometry.points.multiscale.neighbors.back()[static_cast<Size>(new_neighbor)].insert(p);
        }
    }
    //use inc() for incrementing (returns false when not finding a next one)
    if(l_order.inc())//this first if-statement should always return true
    {
        do {
            Size current_point=static_cast<Size>(l_order.pid());
            if (neighbors_of_neighbors.find(current_point)==neighbors_of_neighbors.end())
            { //we only care about finding the neighbors of the neighbors of neighbors
                continue;
            }
            con.compute_cell(cell,l_order);
            std::vector<int> found_neighbors;
            cell.neighbors(found_neighbors);

            //note: negative values given by voro++ refer to the boundaries
            for (int found_neighbor:found_neighbors)
            {
                if (found_neighbor>=0)//voro++ denotes the walls (which we do not need) by negative values
                {
                    Size size_found_neighbor=static_cast<Size>(found_neighbor);
                    //check if already a neighbor (thus no need to add again)
                    if (geometry.points.multiscale.neighbors.back()[current_point].find(size_found_neighbor) ==
                        geometry.points.multiscale.neighbors.back()[current_point].end())
                    {
                    // finally adding a new neighbor for a neighbor of neighbor
                        geometry.points.multiscale.neighbors.back()[current_point].insert(size_found_neighbor);
                        geometry.points.multiscale.neighbors.back()[size_found_neighbor].insert(current_point);
                    }
                }
            }

        } while(l_order.inc());
    }
}


///  Initializes the multi-level procedure
///    @param[in]:  max_coars_lvl: The maximum coarsening level we allow
///    @param[in]:  tol: The tolerance level for which we still consider points similar enough
///    @param[in]:  mgImplementation: The multi-level implementation to use
///                 options: 1) NaiveMG
///                          2) VCycle
///                          3) WCycle
///    @param[in]:  max_n_iterations: The maximum number of iterations the multiresolution scheme is allowed to do
///    @param[in]:  finest_lvl: The finest level the multiresolution procedure will use. If not set to zero (i.e. just using all levels),
///    the resulting converged level populations will be those corresponding to the chosen 'finest_lvl'. This means one has to interpolate these level populations themselves. (using interpolate_levelpops_local)
/////////////////////////////////////////////////////////////////////////////////
inline int Model::setup_multiresolution(Size max_coars_lvl, double tol, Size mgImplementation, Size max_n_iterations, Size finest_lvl)//, string MgImplementation
{
    //Preparing the masks for each level
    geometry.points.multiscale.mask.resize(max_coars_lvl+1);
    //at level 0, the mask is of course all true
    geometry.points.multiscale.mask[0].resize(parameters.npoints());
    for (Size i=0; i<parameters.npoints(); i++)
    {
        geometry.points.multiscale.mask[0][i]=true;
    }

    // I am not sure why i put this to 0
    // parameters.n_off_diag=0;

    //first, we coarsen the grid until we either have too few points left or have too many coarsening levels
    Size new_coars_lvl=1;
    while(new_coars_lvl<=max_coars_lvl)
    // (geometry.points.multiscale.get_total_points(geometry.points.multiscale.get_max_coars_lvl())>min_n_points))
    {std::cout<<"coarsening layer"<<std::endl;
    //options for choosing the tolerance adaptively
    //tol=n*deltatol
        double temp_tol=1-std::pow(1-tol,new_coars_lvl);
        std::cout<<"temp tol: "<<temp_tol<<std::endl;
        coarsen(temp_tol, new_coars_lvl);
        std::cout<<"coarsened layer; number points remaining: "<<geometry.points.multiscale.get_total_points(new_coars_lvl)<<std::endl;
        new_coars_lvl++;
    }
    std::cout<<"finished coarsening"<<std::endl;

    switch (mgImplementation) {
        case 1://"NaiveMG":
        {
            std::shared_ptr<MrController> tempImplement_ptr=std::make_shared<NaiveMG>(geometry.points.multiscale.get_max_coars_lvl()+1,finest_lvl,max_n_iterations);
            mrControllerHelper=MrControllerHelper(tempImplement_ptr);
        }
        break;
        case 2://"VCycle":
        {
            std::shared_ptr<MrController> tempImplement_ptr=std::make_shared<VCycle>(geometry.points.multiscale.get_max_coars_lvl()+1,finest_lvl,1,max_n_iterations);
            mrControllerHelper=MrControllerHelper(tempImplement_ptr);
        }
        break;
        case 3://"WCycle":
        {
            std::shared_ptr<MrController> tempImplement_ptr=std::make_shared<WCycle>(geometry.points.multiscale.get_max_coars_lvl()+1,finest_lvl,1,max_n_iterations);
            mrControllerHelper=MrControllerHelper(tempImplement_ptr);
        }
        break;
        default:
            std::cout<<"The value entered for mgImplementation: "<<mgImplementation<<" does not refer to a valid multiresolution implementation."<<std::endl;
            throw std::runtime_error("Error: " + std::to_string(mgImplementation) +" is not a valid multiresolution implementation argument.");
        break;
    }
    //Initialize structure for previously computed level populations at each level
    computed_level_populations.resize(geometry.points.multiscale.get_max_coars_lvl()+1);
    // And do not forget to set the gpu neighbors after coarsening
    geometry.points.multiscale.set_intern_neighbors();

    return (0);
}


///  Computes the radial basis function
///    @param[in] radius: The input radius
///////////////////////////////////////
template <typename T>
inline T rbf_local(T radius)
{   //exact choice of radial basis function does not seems to matter that much; so we'll stick with the gaussian
    //inverse quadratic
    // return 1.0/(1+std::pow(radius,2));
    //multiquadratic // a tiny bit worse than gaussian in my experience
    // return std::sqrt(1+std::pow(radius,2));

    //gaussian
    return std::exp(-std::pow(radius,2));
}






/// For now, we will create the kd tree on the fly
/// This is due to default initialization (and anything which remotely looks like it) being impossible for these trees, i guess the data just goes out of scope
inline std::tuple<Eigen::Matrix<double, Eigen::Dynamic, 3>, Size1> Model::create_mat_for_kd_tree_of_lvl(Size lvl)
{
    Eigen::Matrix<double, Eigen::Dynamic, 3> temp_mat(geometry.points.multiscale.get_total_points(lvl), 3);
    // std::vector<std::vector<double>> positions;

    Size1 points_at_lvl=geometry.points.multiscale.get_points_at_lvl(lvl);
    // positions.resize(points_at_lvl);
    Size i=0;//just a temporary index
    for (auto point_to_add:points_at_lvl)
    {//add each point to the vector of vectors
        // add x, y and z
        // positions[i].resize();

        // positions[i][0]=geometry.points.position[point_to_add].x();
        // positions[i][1]=geometry.points.position[point_to_add].y();
        // positions[i][2]=geometry.points.position[point_to_add].z();
        temp_mat(i,0)=geometry.points.position[point_to_add].x();
        temp_mat(i,1)=geometry.points.position[point_to_add].y();
        temp_mat(i,2)=geometry.points.position[point_to_add].z();
        i=i+1;
    }

    // For creating the kd_tree, add
    //Set the kd_tree
    // kd_tree mat_index(3, create_mat_for_kd_tree_of_lvl(Size lvl), 10 /* max leaf */);
    // mat_index.index->buildIndex();

    return std::make_tuple(temp_mat,points_at_lvl);
}


///  DEPRECATED, replace usage by Model::get_coarser_neighbors_kd_tree
///  Returns nearby points on the coarser grid
///    @param[in] p: the point around which to find the neighboring points on the coarser grid
///    @param[in] coarser_lvl: the level of the coarser grid
///    @returns : The neighboring points on the coarser grid
inline vector<Size> Model::get_coarser_neighbors(Size p, Size coarser_lvl)
{
  std::set<Size> curr_neighbors=geometry.points.multiscale.get_neighbors(p,coarser_lvl-1);
  //get neighbors in coarser grid
  vector<Size> neighbors_coarser_grid;
  neighbors_coarser_grid.reserve(curr_neighbors.size());//i am overallocating a bit, but this variable is temporary anyway...
  for (Size neighbor: curr_neighbors)
  {
      // if (coarse_mask[neighbor])
      if (geometry.points.multiscale.mask[coarser_lvl][neighbor])
      {
          neighbors_coarser_grid.push_back(neighbor);
      }
  }

  // Note: using the current multiresolution creation method, the number of neighbors in the coarse grid is almost always at least 1
  if (neighbors_coarser_grid.size()==0)//this happens very rarely
  {
      // std::cout<<"No neighbors for the current point! Using point which deleted it instead as neighbor."<<std::endl;
      // std::cout<<"Current point: "<<diff_point<<std::endl;
      // std::cout<<"Point which replaces it: "<<geometry.points.multiscale.point_deleted_map.at(diff_point)<<std::endl;
      // Size repl_point=geometry.points.multiscale.point_deleted_map.at(diff_point);

      neighbors_coarser_grid.push_back(geometry.points.multiscale.point_deleted_map.at(p));
  }

  // In the case we do not really have enough points for a good interpolation, just add more
  if (neighbors_coarser_grid.size()<MIN_INTERPOLATION_POINTS)
  {
      std::set<Size> temp_neighbors_coarser_grid;
      for (Size neighbor_coarse: neighbors_coarser_grid)
      {
          for (Size neighbor_of_coarse_neighbor: geometry.points.multiscale.get_neighbors(neighbor_coarse,coarser_lvl))
          {   //just make sure that we do not accidentally insert a point we already have (in neighbors_coarser_grid)
              if (std::find(neighbors_coarser_grid.begin(), neighbors_coarser_grid.end(), neighbor_of_coarse_neighbor) == neighbors_coarser_grid.end())
              {
                  temp_neighbors_coarser_grid.insert(neighbor_of_coarse_neighbor);
              }
          }
      }
      //Finally add our new points too
      std::copy(temp_neighbors_coarser_grid.begin(), temp_neighbors_coarser_grid.end(), std::back_inserter(neighbors_coarser_grid));
  }
  if (neighbors_coarser_grid.size()>MAX_INTERPOLATION_POINTS)
  {//now this just becomes: 1) to expensive to calculate and 2)difference between distances might become too large. So we remove some points
      Size maxnbneighbors=MAX_INTERPOLATION_POINTS;
      Size nbneighbors=neighbors_coarser_grid.size();
      vector<double> distances2;//stores the distances squared of the n closest points
      distances2.resize(maxnbneighbors);
      std::fill(distances2.begin(), distances2.end(), std::numeric_limits<double>::max());
      vector<Size> closest_points;//stores the n closest points
      double maxdist=std::numeric_limits<double>::max();//maximum distance of the n closest points
      Size maxindex=0;//corresponding index
      closest_points.resize(maxnbneighbors);
      std::fill(closest_points.begin(), closest_points.end(), parameters.npoints());
      for (Size idx=0;idx<nbneighbors;idx++)
      {
          double tempdistance=(geometry.points.position[neighbors_coarser_grid[idx]]-geometry.points.position[p]).squaredNorm();
          if (tempdistance<maxdist)
          {   //replace the curr max distance
              closest_points[maxindex]=neighbors_coarser_grid[idx];
              distances2[maxindex]=tempdistance;
              //get new max distance and corresponding max index
              auto tempindex = std::max_element(distances2.begin(), distances2.end());
              maxindex=tempindex - distances2.begin();
              maxdist=distances2[maxindex];
          }
      }
      neighbors_coarser_grid=closest_points;
  }
  return neighbors_coarser_grid;
}

/// Returns the closest points for point p on the coarser grid as defined by the matrix generationg the kd tree
///   @param[in] p: index of point for which to find the closest neighbors in a coarser grid (?TODO? I do not know whether you are allowed to choose a point already in the coarser grid, so please refrain from doing so)
///   @param[in] kdtree: The kd tree generated using the coarser grid (just use Model::create_mat_for_kd_tree_of_lvl)
///   @param[in] index_conversion: vector for converting from 'compressed' index (corresponding to the coarser grid) to the regular index
///   @Returns: a vector with indices corresponding to the nearest points on the coarser grid
inline vector<Size> Model::get_coarser_neighbors_kd_tree(Size p, kd_tree& kdtree, Size1& index_conversion)
{
    std::vector<double> query_pt(3);
    query_pt[0]=geometry.points.position[p].x();
    query_pt[1]=geometry.points.position[p].y();
    query_pt[2]=geometry.points.position[p].z();

    Size nb_neighbors_to_query;//TODO: maybe allow us to choose another amount of neighbors

    if (parameters.spherical_symmetry())
    {
        nb_neighbors_to_query=INTERPOLATION_POINTS_1D;
    }
    else
    {
        nb_neighbors_to_query=INTERPOLATION_POINTS_3D;
    }

    //TODO: locally define something for replacing the n usages of MIN_INTERPOLATION_POINTS

    // Note to self, nanoflann requires size_t (unsigned int), so that is why we do not use our Size (long unsinged int) here
    vector<size_t> ret_indexes(nb_neighbors_to_query);//still need to be translated to the actual indices
    vector<double> out_dists_sqr(nb_neighbors_to_query);//note: we are not really using this, but nanoflann requires it

    nanoflann::KNNResultSet<double> resultSet(nb_neighbors_to_query);

    resultSet.init(&ret_indexes[0], &out_dists_sqr[0]);
    kdtree.index->findNeighbors(resultSet, &query_pt[0],
                                 nanoflann::SearchParams(10));

    vector<Size> true_indices;
    true_indices.resize(nb_neighbors_to_query);
    for (size_t i=0; i<nb_neighbors_to_query; i++)
    {
        true_indices[i]=index_conversion[ret_indexes[i]];
        // std::cout<<"for point "<<p<<" found neighbor: "<<true_indices[i]<<std::endl;
    }
    return true_indices;
}


//
///  Interpolates the relative differences of level populations (linearized matrix) from the coarser level to the first finer level
///  The result will be stored in relative_difference_levelpopulations
///    @param[in] coarser_lvl: The coarsening level of the coarser grid
///    @param[in,out] relative_difference_levelpopulations: The relative differences for the level populations (line species, lineProducingSpecies.index(point, level))
///  NOTE: with some better data structure, we can remove some duplication between the interpolation functions
//////////////////////////////////////////////////////////////////////////////////////////////////////////////
inline void Model::interpolate_relative_differences_local(Size coarser_lvl, Size finer_lvl, vector<VectorXr> &relative_difference_levelpopulations)
{
    //we cannot interpolate from lvl 0 to some level -..., so just return
    if (coarser_lvl==0)
    {return;}
    std::cout<<"Starting the interpolation"<<std::endl;
    Size n_points=parameters.npoints();

    // Vector<unsigned char> coarse_mask=geometry.points.multiscale.get_mask(coarser_lvl);
    // Vector<unsigned char> finer_mask=geometry.points.multiscale.get_mask(coarser_lvl-1);

    vector<Size> diff_points;

    for (Size point=0; point<n_points; point++)
    {
        // if(finer_mask[point]&&!coarse_mask[point])
        if((geometry.points.multiscale.mask[finer_lvl][point])&&!(geometry.points.multiscale.mask[coarser_lvl][point]))
        {
            diff_points.push_back(point);
        }
    }

    // Size n_coarse_points=coarse_points.size();
    Size n_diff_points=diff_points.size();

    //if we have truly nothing to do, just do nothing
    if (n_diff_points==0)
    {return;}

    //Set the kd_tree
    Eigen::Matrix<double, Eigen::Dynamic, 3> temp_mat;
    Size1 index_conversion;
    auto tuple=create_mat_for_kd_tree_of_lvl(coarser_lvl);
    temp_mat=std::get<0>(tuple);
    index_conversion=std::get<1>(tuple);
    kd_tree mat_index(3, temp_mat, 10 /* max leaf */);
    mat_index.index->buildIndex();

    //for every points in diff_points, try to find interpolating value using rbf function
    for (Size diff_point: diff_points)
    {
        // vector<Size> neighbors_coarser_grid=get_coarser_neighbors(diff_point,coarser_lvl);
        vector<Size> neighbors_coarser_grid=get_coarser_neighbors_kd_tree(diff_point, mat_index, index_conversion);

        Size n_neighbors_coarser_grid=neighbors_coarser_grid.size();

        Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic> rbf_mat(n_neighbors_coarser_grid, n_neighbors_coarser_grid);
        Eigen::Matrix<double,1,Eigen::Dynamic> distance_with_neighbors(1,n_neighbors_coarser_grid);

        for (Size idx=0; idx<n_neighbors_coarser_grid; idx++)
        {
            distance_with_neighbors(idx)=std::sqrt((geometry.points.position[neighbors_coarser_grid[idx]]-geometry.points.position[diff_point]).squaredNorm());
            rbf_mat(idx,idx)=0;//distance between point and itself is zero
            for (Size idx2=0; idx2<idx; idx2++)
            {
                //calculate radius
                double radius=std::sqrt((geometry.points.position[neighbors_coarser_grid[idx]]-geometry.points.position[neighbors_coarser_grid[idx2]]).squaredNorm());
                rbf_mat(idx,idx2)=radius;
                rbf_mat(idx2,idx)=radius;
            }
        }
        //just use the mean distance for less parameter tuning
        double meandist=distance_with_neighbors.mean();

        rbf_mat=rbf_mat/meandist;
        rbf_mat=rbf_mat.unaryExpr(std::ptr_fun(rbf_local<double>));

        distance_with_neighbors=distance_with_neighbors/meandist;
        distance_with_neighbors=distance_with_neighbors.unaryExpr(std::ptr_fun(rbf_local<double>));

        // Going with ColPivHouseholderQR for simplicity and accuracy
        // Eigen::LDLT<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic>> ldltdec(rbf_mat);
        // Technically, when using a gaussian RBF, the matrix should be positive definite:  see e.g. Fornberg and Flyer (2015). "Solving PDEs with radial basis functions"
        // But numerical nonsense can always occur (and the current implementation is fast enough)
        Eigen::ColPivHouseholderQR<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic>> colPivHouseholderQr(rbf_mat);
        //now that we have our decomposition, calculate the interpolated value
        for (Size specidx=0; specidx<parameters.nlspecs(); specidx++)
        {
            // For finding out which abundance corresponds to to the current species
            Size speciesnum=lines.lineProducingSpecies[specidx].linedata.num;
            // We will need to calculate the relative differences, so we collect them in a vector
            vector<Real> rel_diff_of_point;
            rel_diff_of_point.resize(lines.lineProducingSpecies[specidx].linedata.nlev);
            for (Size levidx=0; levidx<lines.lineProducingSpecies[specidx].linedata.nlev; levidx++)
            {
                Eigen::Vector<double,Eigen::Dynamic> right_hand_side(n_neighbors_coarser_grid);
                for (Size idx=0; idx<n_neighbors_coarser_grid; idx++)
                {
                    right_hand_side(idx)=static_cast<double>(relative_difference_levelpopulations[specidx](lines.lineProducingSpecies[specidx].index(neighbors_coarser_grid[idx],levidx)));
                }
                Eigen::Vector<double,Eigen::Dynamic> weights=colPivHouseholderQr.solve(right_hand_side);
                Real interpolated_value=static_cast<Real>((distance_with_neighbors*weights)(0,0));//just get the single value in this 1-element 'matrix'

                rel_diff_of_point[levidx]=interpolated_value;
                if (std::isnan(interpolated_value)||std::isinf(interpolated_value))//FIXME: also check for the potential rare case of getting negative levelpops
                {
                    std::cout<<"Something went wrong during interpolating: nan/inf value occuring"<<std::endl;
                    throw std::runtime_error("Nan/inf encountered during interpolation");
                }
            }// end of iterating over lines of species

            //and now we finally set the values
            for (Size levidx=0; levidx<lines.lineProducingSpecies[specidx].linedata.nlev; levidx++)
            {
                relative_difference_levelpopulations[specidx](lines.lineProducingSpecies[specidx].index(diff_point,levidx))=static_cast<Real>(rel_diff_of_point[levidx]);
            }
        }
    }
}







///  Interpolates the level populations (linearized matrix) from the coarser level to the next finer level
///    @param[in]: coarser_lvl: the coarsening level of the coarser grid
/// NOTE: with some better data structure, we can remove some duplication between the interpolation functions
/////////////////////////////////////////////////////////////////////////////////////////////////////////////
inline void Model::interpolate_levelpops_local(Size coarser_lvl, Size finer_lvl)
{
    //we cannot interpolate from lvl 0 to some level -1, so just return
    if (coarser_lvl==0)
    {return;}
    std::cout<<"Starting the interpolation"<<std::endl;
    Size n_points=parameters.npoints();

    // Vector<unsigned char> coarse_mask=geometry.points.multiscale.get_mask(coarser_lvl);
    // Vector<unsigned char> finer_mask=geometry.points.multiscale.get_mask(coarser_lvl-1);

    vector<Size> diff_points;

    for (Size point=0; point<n_points; point++)
    {
        // if(finer_mask[point]&&!coarse_mask[point])
        if(geometry.points.multiscale.mask[finer_lvl][point]&&!geometry.points.multiscale.mask[coarser_lvl][point])
        {
            diff_points.push_back(point);
        }
    }

    Size n_diff_points=diff_points.size();

    //if we have truly nothing to do, just do nothing
    if (n_diff_points==0)
    {return;}


    //Set the kd_tree
    Eigen::Matrix<double, Eigen::Dynamic, 3> temp_mat;
    Size1 index_conversion;
    auto tuple=create_mat_for_kd_tree_of_lvl(coarser_lvl);
    temp_mat=std::get<0>(tuple);
    index_conversion=std::get<1>(tuple);
    kd_tree mat_index(3, temp_mat, 10 /* max leaf */);
    mat_index.index->buildIndex();

    //for every points in diff_points, try to find interpolating value using rbf function
    for (Size diff_point: diff_points)
    {
        // vector<Size> neighbors_coarser_grid=get_coarser_neighbors(diff_point,coarser_lvl);
        vector<Size> neighbors_coarser_grid=get_coarser_neighbors_kd_tree(diff_point, mat_index, index_conversion);

        Size n_neighbors_coarser_grid=neighbors_coarser_grid.size();

        Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic> rbf_mat(n_neighbors_coarser_grid, n_neighbors_coarser_grid);
        Eigen::Matrix<double,1,Eigen::Dynamic> distance_with_neighbors(1,n_neighbors_coarser_grid);

        for (Size idx=0; idx<n_neighbors_coarser_grid; idx++)
        {
            distance_with_neighbors(idx)=std::sqrt((geometry.points.position[neighbors_coarser_grid[idx]]-geometry.points.position[diff_point]).squaredNorm());
            rbf_mat(idx,idx)=0;//distance between point and itself is zero
            for (Size idx2=0; idx2<idx; idx2++)
            {
                //calculate radius
                double radius=std::sqrt((geometry.points.position[neighbors_coarser_grid[idx]]-geometry.points.position[neighbors_coarser_grid[idx2]]).squaredNorm());
                rbf_mat(idx,idx2)=radius;
                rbf_mat(idx2,idx)=radius;
            }
        }
        //just use the mean distance for less parameter tuning
        double meandist=distance_with_neighbors.mean();

        rbf_mat=rbf_mat/meandist;
        rbf_mat=rbf_mat.unaryExpr(std::ptr_fun(rbf_local<double>));

        distance_with_neighbors=distance_with_neighbors/meandist;
        distance_with_neighbors=distance_with_neighbors.unaryExpr(std::ptr_fun(rbf_local<double>));

        // Going with ColPivHouseholderQR for simplicity and accuracy
        // Eigen::LDLT<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic>> ldltdec(rbf_mat);
        // Technically, when using a gaussian RBF, the matrix should be positive definite:  see e.g. Fornberg and Flyer (2015). "Solving PDEs with radial basis functions"
        // But numerical nonsense can always occur (and the current implementation is fast enough)
        Eigen::ColPivHouseholderQR<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic>> colPivHouseholderQr(rbf_mat);
        //now that we have our decomposition, calculate the interpolated value
        for (Size specidx=0; specidx<parameters.nlspecs(); specidx++)
        {
            // For finding out which abundance corresponds to to the current species
            Size speciesnum=lines.lineProducingSpecies[specidx].linedata.num;
            // We will need to renormalize the level pops, so first we should collect them
            vector<Real> linefracs;
            linefracs.resize(lines.lineProducingSpecies[specidx].linedata.nlev);
            for (Size levidx=0; levidx<lines.lineProducingSpecies[specidx].linedata.nlev; levidx++)
            {
                Eigen::Vector<double,Eigen::Dynamic> right_hand_side(n_neighbors_coarser_grid);
                for (Size idx=0; idx<n_neighbors_coarser_grid; idx++)
                {
                    // interpolating the fractional level populations, so we need the abundance of each species
                    double abund=chemistry.species.abundance[neighbors_coarser_grid[idx]][speciesnum];
                    // interpolating fractional level populations
                    right_hand_side(idx)=static_cast<double>(lines.lineProducingSpecies[specidx].get_level_pop(neighbors_coarser_grid[idx],levidx))/abund;
                }
                Eigen::Vector<double,Eigen::Dynamic> weights=colPivHouseholderQr.solve(right_hand_side);
                Real interpolated_value=static_cast<Real>((distance_with_neighbors*weights)(0,0));

                linefracs[levidx]=interpolated_value;
                if (std::isnan(interpolated_value)||std::isinf(interpolated_value))//FIXME: also check for the potential rare case of getting negative levelpops
                {
                    std::cout<<"Something went wrong during interpolating: nan/inf value occuring"<<std::endl;
                    throw std::runtime_error("Nan/inf encountered during interpolation");
                }
            }// end of iterating over lines of species
            //Now first set negative values to 0 (because we do not want negative level populations)
            for (Size i=0; i<linefracs.size(); i++)
            {
                if (linefracs[i]<0){linefracs[i]=0;}
            }
            // Linefracs should sum to 1, otherwise divide by the sum
            Real sum_of_linefracs=std::accumulate(linefracs.begin(), linefracs.end(), 0.0);

            if (sum_of_linefracs==0)
            {
                //TODO: add fallback values in the unlikely case that all interpolated levelpops are negative
                std::cout<<"Something went wrong during interpolating: all interpolated linefracs were negative"<<std::endl;
                throw std::runtime_error("all interpolated linefracs were negative during interpolation");
            }

            //and now we finally set the values
            Real diff_point_abund=static_cast<Real>(chemistry.species.abundance[diff_point][speciesnum]);
            for (Size levidx=0; levidx<lines.lineProducingSpecies[specidx].linedata.nlev; levidx++)
            {
                lines.lineProducingSpecies[specidx].set_level_pop(diff_point,levidx,diff_point_abund*(linefracs[levidx]/sum_of_linefracs));
            }
        }
    }
}




// ///  Interpolates the averaged intensities (J_eff and J_diff) from the coarser level to the next finer level
// ///    @param[in]: coarser_lvl: the coarsening level of the coarser grid
// /// NOTE: with some better data structure, we can remove some duplication between the interpolation functions
// /////////////////////////////////////////////////////////////////////////////////////////////////////////////
// inline void Model::interpolate_intensities_local(Size coarser_lvl)
// {
//     //we cannot interpolate from lvl 0 to some level -1, so just return
//     if (coarser_lvl==0)
//     {return;}
//     std::cout<<"Starting the interpolation"<<std::endl;
//     Size n_points=parameters.npoints();
//
//     // Vector<unsigned char> coarse_mask=geometry.points.multiscale.get_mask(coarser_lvl);
//     // Vector<unsigned char> finer_mask=geometry.points.multiscale.get_mask(coarser_lvl-1);
//
//     vector<Size> diff_points;
//
//     for (Size point=0; point<n_points; point++)
//     {
//         // if(finer_mask[point]&&!coarse_mask[point])
//         if(geometry.points.multiscale.mask[coarser_lvl-1][point]&&!geometry.points.multiscale.mask[coarser_lvl][point])
//         {
//             diff_points.push_back(point);
//         }
//     }
//
//     Size n_diff_points=diff_points.size();
//
//     //if we have truly nothing to do, just do nothing
//     if (n_diff_points==0)
//     {return;}
//
//     //for every point in diff_points, try to find interpolating value using rbf function
//     for (Size diff_point: diff_points)
//     {
//         vector<Size> neighbors_coarser_grid=get_coarser_neighbors(diff_point,coarser_lvl);
//
//         Size n_neighbors_coarser_grid=neighbors_coarser_grid.size();
//
//         Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic> rbf_mat(n_neighbors_coarser_grid, n_neighbors_coarser_grid);
//         Eigen::Matrix<double,1,Eigen::Dynamic> distance_with_neighbors(1,n_neighbors_coarser_grid);
//
//         for (Size idx=0; idx<n_neighbors_coarser_grid; idx++)
//         {
//             distance_with_neighbors(idx)=std::sqrt((geometry.points.position[neighbors_coarser_grid[idx]]-geometry.points.position[diff_point]).squaredNorm());
//             rbf_mat(idx,idx)=0;//distance between point and itself is zero
//             for (Size idx2=0; idx2<idx; idx2++)
//             {
//                 //calculate radius
//                 double radius=std::sqrt((geometry.points.position[neighbors_coarser_grid[idx]]-geometry.points.position[neighbors_coarser_grid[idx2]]).squaredNorm());
//                 rbf_mat(idx,idx2)=radius;
//                 rbf_mat(idx2,idx)=radius;
//             }
//         }
//         //just use the mean distance for less parameter tuning
//         double meandist=distance_with_neighbors.mean();
//
//         rbf_mat=rbf_mat/meandist;
//         rbf_mat=rbf_mat.unaryExpr(std::ptr_fun(rbf_local<double>));
//
//         distance_with_neighbors=distance_with_neighbors/meandist;
//         distance_with_neighbors=distance_with_neighbors.unaryExpr(std::ptr_fun(rbf_local<double>));
//
//         // Going with ColPivHouseholderQR for simplicity and accuracy
//         // Eigen::LDLT<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic>> ldltdec(rbf_mat);
//         // Technically, when using a gaussian RBF, the matrix should be positive definite:  see e.g. Fornberg and Flyer (2015). "Solving PDEs with radial basis functions"
//         // But numerical nonsense can always occur (and the current implementation is fast enough)
//         Eigen::ColPivHouseholderQR<Eigen::Matrix<double,Eigen::Dynamic,Eigen::Dynamic>> colPivHouseholderQr(rbf_mat);
//         //now that we have our decomposition, calculate the interpolated value
//         for (Size specidx=0; specidx<parameters.nlspecs(); specidx++)
//         {
//             // For finding out which abundance corresponds to to the current species
//             Size speciesnum=lines.lineProducingSpecies[specidx].linedata.num;
//             // We will interpolate the Jeff for all transitions
//             vector<Real> Jeffs;
//             vector<Real> Jdifs;
//             Jeffs.resize(lines.lineProducingSpecies[specidx].linedata.nrad);
//             Jdifs.resize(lines.lineProducingSpecies[specidx].linedata.nrad);
//             //of course, we will calculate this for all radiative transitions
//             for (Size radidx=0; radidx<lines.lineProducingSpecies[specidx].linedata.nrad; radidx++)
//             {
//                 Eigen::Vector<double,Eigen::Dynamic> right_hand_side_jeff(n_neighbors_coarser_grid);
//                 Eigen::Vector<double,Eigen::Dynamic> right_hand_side_jdif(n_neighbors_coarser_grid);
//                 for (Size idx=0; idx<n_neighbors_coarser_grid; idx++)
//                 {
//                     // RHS for interpolating Jeff
//                     right_hand_side_jeff(idx)=static_cast<double>(lines.lineProducingSpecies[specidx].Jeff[neighbors_coarser_grid[idx]][radidx]);
//                     right_hand_side_jdif(idx)=static_cast<double>(lines.lineProducingSpecies[specidx].Jdif[neighbors_coarser_grid[idx]][radidx]);
//                 }
//                 Eigen::Vector<double,Eigen::Dynamic> weights_jeff=colPivHouseholderQr.solve(right_hand_side_jeff);
//                 Eigen::Vector<double,Eigen::Dynamic> weights_jdif=colPivHouseholderQr.solve(right_hand_side_jdif);
//
//                 Real interpolated_value_jeff=static_cast<Real>((distance_with_neighbors*weights_jeff)(0,0));
//                 Real interpolated_value_jdif=static_cast<Real>((distance_with_neighbors*weights_jdif)(0,0));
//
//                 Jeffs[radidx]=interpolated_value_jeff;
//                 Jdifs[radidx]=interpolated_value_jdif;
//                 if (std::isnan(interpolated_value_jeff)||std::isinf(interpolated_value_jeff)||std::isnan(interpolated_value_jdif)||std::isinf(interpolated_value_jdif))//FIXME: also check for the potential rare case of getting negative levelpops
//                 {
//                     std::cout<<"Something went wrong during interpolating: nan/inf value occuring"<<std::endl;
//                     throw std::runtime_error("Nan/inf encountered during interpolation");
//                 }
//             }// end of iterating over radiative transitions of species
//             //TODO remove: Now first set negative values to 0 (because we do not want negative level populations)
//             // for (Size i=0; i<linefracs.size(); i++)
//             // {
//             //     if (linefracs[i]<0){linefracs[i]=0;}
//             // }
//
//             //and now we finally set the values
//             // Real diff_point_abund=chemistry.species.abundance[diff_point][speciesnum];
//             for (Size radidx=0; radidx<lines.lineProducingSpecies[specidx].linedata.nrad; radidx++)
//             {
//                 lines.lineProducingSpecies[specidx].Jeff[diff_point][radidx]=Jeffs[radidx];
//                 lines.lineProducingSpecies[specidx].Jdif[diff_point][radidx]=Jdifs[radidx];
//             }
//         }
//     }
// }
