inline void Solver :: trace (Model& model)
{
    const Size hnrays  = model.geometry.rays  .get_nrays  ()/2;
    const Size npoints = model.geometry.points.get_npoints();

    Solver* solver_cpy = (Solver*) pc::accelerator::malloc (sizeof(Solver));
     Model*  model_cpy = ( Model*) pc::accelerator::malloc (sizeof( Model));

    pc::accelerator::memcpy_to_accelerator (solver_cpy,  this,  sizeof(Solver));
    pc::accelerator::memcpy_to_accelerator ( model_cpy, &model, sizeof( Model));

    for (Size rr = 0; rr < hnrays; rr++)
    {
        const Size ar = model.geometry.rays.antipod.vec[rr];

        cout << "rr = " << rr << endl;

        accelerated_for (o, 10000, nblocks, nthreads,
        {
            const Real dshift_max = 1.0e+99;

            model_cpy->geometry.lengths[npoints*rr+o]  =
                solver_cpy->trace_ray <CoMoving> (model_cpy->geometry, o, rr, dshift_max, +1);
              - solver_cpy->trace_ray <CoMoving> (model_cpy->geometry, o, ar, dshift_max, -1);
        })
    }

    model.geometry.lengths.copy_ptr_to_vec ();
}


template <Frame frame>
accel inline Size Solver :: trace_ray (
    const Geometry& geometry,
    const Size      o,
    const Size      r,
    const Real      dshift_max,
    const int       increment )
{
    Size id = centre;   // index distance from origin
    Real  Z = 0.0;      // distance from origin (o)
    Real dZ = 0.0;      // last increment in Z

    Size nxt = geometry.get_next (o, r, o, Z, dZ);

    if (nxt != geometry.get_npoints()) // if we are not going out of mesh
    {
        Size       crt = o;
        Real shift_crt = geometry.get_shift <frame> (o, r, crt);
        Real shift_nxt = geometry.get_shift <frame> (o, r, nxt);

        set_data (crt, nxt, shift_crt, shift_nxt, dZ, dshift_max, increment, id);

        while (geometry.boundary.point2boundary[nxt] == geometry.get_npoints()) // while nxt not on boundary
        {
                  crt =       nxt;
            shift_crt = shift_nxt;
                  nxt = geometry.get_next (o, r, nxt, Z, dZ);
            if (nxt == geometry.get_npoints()) printf ("ERROR (nxt < 0): o = %ld, crt = %ld, ray = %ld", o, crt, r);
            shift_nxt = geometry.get_shift <frame> (o, r, nxt);

            set_data (crt, nxt, shift_crt, shift_nxt, dZ, dshift_max, increment, id);
        }
    }

    return id;
}


accel inline void Solver :: set_data (
    const Size  crt,
    const Size  nxt,
    const Real  shift_crt,
    const Real  shift_nxt,
    const Real  dZ_loc,
    const Real  dshift_max,
    const int   increment,
          Size& id )
{
    const Real dshift     = shift_nxt - shift_crt;
    const Real dshift_abs = fabs (dshift);

    if (dshift_abs > dshift_max) // If velocity gradient is not well-sampled enough
    {
        // Interpolate velocity gradient field
        const Real      n_interpl = dshift_abs / dshift_max + Real (1);
        const Real half_n_interpl = Real (0.5) * n_interpl;
        const Real     dZ_interpl =     dZ_loc / n_interpl;
        const Real dshift_interpl =     dshift / n_interpl;

        if (n_interpl > 10000)
        {
            printf ("ERROR (N_intpl > 10 000) || (dshift_max < 0, probably due to overflow)\n");
        }

        // Assign current cell to first half of interpolation points
        for (Size m = 1; m < half_n_interpl; m++)
        {
               nr[id] = crt;
            shift[id] = shift_crt + m*dshift_interpl;
               dZ[id] = dZ_interpl;

            id += increment;
        }

        // Assign next cell to second half of interpolation points
        for (Size m = half_n_interpl; m <= n_interpl; m++)
        {
               nr[id] = nxt;
            shift[id] = shift_crt + m*dshift_interpl;
               dZ[id] = dZ_interpl;

            id += increment;
        }
    }

    else
    {
           nr[id] = nxt;
        shift[id] = shift_nxt;
           dZ[id] = dZ_loc;

           id += increment;
    }
}
