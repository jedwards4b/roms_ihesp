      MODULE ocean_control_mod
!
!svn $Id: tl_w4dvar_ocean.h 553 2011-04-22 21:30:04Z arango $
!=================================================== Andrew M. Moore ===
!  Copyright (c) 2002-2011 The ROMS/TOMS Group      Hernan G. Arango   !
!    Licensed under a MIT/X style license                              !
!    See License_ROMS.txt                                              !
!=======================================================================
!                                                                      !
!  ROMS/TOMS Weak Constraint 4-Dimensional Variational (4DVar) Data    !
!            Assimilation  and its  Tangent Linear Driver: Indirect    !
!            Representer Approach                                      !
!                                                                      !
!  This driver is used for weak constraint 4DVar where errors are      !
!  considered in both model and observations.                          !
!                                                                      !
!  The routines in this driver control the initialization,  time-      !
!  stepping, and finalization of  ROMS/TOMS  model following ESMF      !
!  conventions:                                                        !
!                                                                      !
!     ROMS_initialize                                                  !
!     ROMS_run                                                         !
!     ROMS_finalize                                                    !
!                                                                      !
!=======================================================================
!
      implicit none

      PRIVATE
      PUBLIC  :: ROMS_initialize
      PUBLIC  :: ROMS_run
      PUBLIC  :: ROMS_finalize

      CONTAINS

      SUBROUTINE ROMS_initialize (first, mpiCOMM)
!
!=======================================================================
!                                                                      !
!  This routine allocates and initializes ROMS/TOMS state variables    !
!  and internal and external parameters.                               !
!                                                                      !
!=======================================================================
!
      USE mod_param
      USE mod_parallel
      USE mod_fourdvar
      USE mod_iounits
      USE mod_scalars

#ifdef MCT_LIB
!
# ifdef AIR_OCEAN
      USE ocean_coupler_mod, ONLY : initialize_ocn2atm_coupling
# endif
# ifdef WAVES_OCEAN
      USE ocean_coupler_mod, ONLY : initialize_ocn2wav_coupling
# endif
#endif
!
!  Imported variable declarations.
!
      logical, intent(inout) :: first

      integer, intent(in), optional :: mpiCOMM
!
!  Local variable declarations.
!
      logical :: allocate_vars = .TRUE.

#ifdef DISTRIBUTE
      integer :: MyError, MySize
#endif
      integer :: STDrec, Tindex, ng, thread

#ifdef DISTRIBUTE
!
!-----------------------------------------------------------------------
!  Set distribute-memory (MPI) world communictor.
!-----------------------------------------------------------------------
!
      IF (PRESENT(mpiCOMM)) THEN
        OCN_COMM_WORLD=mpiCOMM
      ELSE
        OCN_COMM_WORLD=MPI_COMM_WORLD
      END IF
      CALL mpi_comm_rank (OCN_COMM_WORLD, MyRank, MyError)
      CALL mpi_comm_size (OCN_COMM_WORLD, MySize, MyError)
#endif
!
!-----------------------------------------------------------------------
!  On first pass, initialize model parameters a variables for all
!  nested/composed grids.  Notice that the logical switch "first"
!  is used to allow multiple calls to this routine during ensemble
!  configurations.
!-----------------------------------------------------------------------
!
      IF (first) THEN
        first=.FALSE.
!
!  Initialize parallel control switches. These scalars switches are
!  independent from standard input parameters.
!
        CALL initialize_parallel
!
!  Read in model tunable parameters from standard input. Allocate and
!  initialize variables in several modules after the number of nested
!  grids and dimension parameters are known.
!
        CALL inp_par (iNLM)
        IF (exit_flag.ne.NoError) RETURN
!
!  Initialize internal wall clocks. Notice that the timings does not
!  includes processing standard input because several parameters are
!  needed to allocate clock variables.
!
        IF (Master) THEN
          WRITE (stdout,10)
 10       FORMAT (/,' Process Information:',/)
        END IF
!
        DO ng=1,Ngrids
!$OMP PARALLEL DO PRIVATE(thread) SHARED(numthreads)
          DO thread=0,numthreads-1
            CALL wclock_on (ng, iNLM, 0)
          END DO
!$OMP END PARALLEL DO
        END DO
!
!  Allocate and initialize modules variables.
!
        CALL mod_arrays (allocate_vars)
!
!  Allocate and initialize observation arrays.
!
        CALL initialize_fourdvar

      END IF

#if defined MCT_LIB && (defined AIR_OCEAN || defined WAVES_OCEAN)
!
!-----------------------------------------------------------------------
!  Initialize coupling streams between model(s).
!-----------------------------------------------------------------------
!
      DO ng=1,Ngrids
# ifdef AIR_OCEAN
        CALL initialize_ocn2atm_coupling (ng, MyRank)
# endif
# ifdef WAVES_OCEAN
        CALL initialize_ocn2wav_coupling (ng, MyRank)
# endif
      END DO
#endif
!
!-----------------------------------------------------------------------
!  Read in standard deviation factors for error covariance.
!-----------------------------------------------------------------------
!
!  Initial conditions standard deviation. They are loaded in Tindex=1
!  of the e_var(...,Tindex) state variables.
!
      STDrec=1
      Tindex=1
      DO ng=1,Ngrids
        CALL get_state (ng, 6, 6, STD(1,ng)%name, STDrec, Tindex)
        IF (exit_flag.ne.NoError) RETURN
      END DO
!
!  Model error standard deviation. They are loaded in Tindex=2
!  of the e_var(...,Tindex) state variables.
!
      STDrec=1
      Tindex=2
      IF (NSA.eq.2) THEN
        DO ng=1,Ngrids
          CALL get_state (ng, 6, 6, STD(2,ng)%name, STDrec, Tindex)
          IF (exit_flag.ne.NoError) RETURN
        END DO
      END IF

#ifdef ADJUST_BOUNDARY
!
!  Open boundary conditions standard deviation.
!
      STDrec=1
      Tindex=1
      DO ng=1,Ngrids
        CALL get_state (ng, 8, 8, STD(3,ng)%name, STDrec, Tindex)
        IF (exit_flag.ne.NoError) RETURN
      END DO
#endif
#if defined ADJUST_WSTRESS || defined ADJUST_STFLUX
!
!  Surface forcing standard deviation.
!
      STDrec=1
      Tindex=1
      DO ng=1,Ngrids
        CALL get_state (ng, 9, 9, STD(4,ng)%name, STDrec, Tindex)
        IF (exit_flag.ne.NoError) RETURN
      END DO
#endif

      RETURN
      END SUBROUTINE ROMS_initialize

      SUBROUTINE ROMS_run (RunInterval)
!
!=======================================================================
!                                                                      !
!  This routine time-steps ROMS/TOMS nonlinear, tangent linear and     !
!  adjoint models.                                                     !
!                                                                      !
!=======================================================================
!
      USE mod_param
      USE mod_parallel
      USE mod_fourdvar
      USE mod_iounits
      USE mod_ncparam
      USE mod_netcdf
      USE mod_scalars
      USE mod_stepping
!
      USE convolve_mod, ONLY : error_covariance
#ifdef ADJUST_BOUNDARY
      USE mod_boundary, ONLY : initialize_boundary
#endif
      USE mod_forces, ONLY : initialize_forces
      USE mod_ocean, ONLY : initialize_ocean
      USE normalization_mod, ONLY : normalization
      USE strings_mod, ONLY : uppercase
#if defined BALANCE_OPERATOR && defined ZETA_ELLIPTIC
      USE zeta_balance_mod, ONLY: balance_ref, biconj
#endif
!
!  Imported variable declarations
!
      real(r8), intent(in) :: RunInterval            ! seconds
!
!  Local variable declarations.
!
      logical :: Lcgini, Linner, Lposterior

      integer :: my_inner, my_outer
      integer :: Lbck, Lini, Rec, Rec1, Rec2
      integer :: i, ng, status, subs, tile, thread
      integer :: Fcount, Mstr, NRMrec

      integer, dimension(Ngrids) :: Nrec

      character (len=9 ) :: driver
      character (len=20) :: string
!
!=======================================================================
!  Run model for all nested grids, if any.
!=======================================================================
!
!  Initialize relevant parameters.
!
      DO ng=1,Ngrids
#if defined ADJUST_STFLUX || defined ADJUST_WSTRESS
        Lfinp(ng)=1         ! forcing index for input
        Lfout(ng)=1         ! forcing index for output history files
#endif
#ifdef ADJUST_BOUNDARY
        Lbinp(ng)=1         ! boundary index for input
        Lbout(ng)=1         ! boundary index for output history files
#endif
        Lold(ng)=1          ! old minimization time index
        Lnew(ng)=2          ! new minimization time index
      END DO
      Lini=1                ! NLM initial conditions record in INI
      Lbck=2                ! background record in INI
      Rec1=1
      Rec2=2
      Nrun=1
      outer=0
      inner=0
      ERstr=1
      ERend=Nouter
      driver='tl_w4dvar'
!
!-----------------------------------------------------------------------
!  Configure weak constraint 4DVAR algorithm: Indirect Representer
!  Approach.
!-----------------------------------------------------------------------
!
!  Initialize the switch to gather weak constraint forcing.
!
      DO ng=1,Ngrids
        WRTforce(ng)=.FALSE.
      END DO
!
!  Initialize and set nonlinear model initial conditions.
!
      DO ng=1,Ngrids
        wrtNLmod(ng)=.TRUE.
        wrtRPmod(ng)=.FALSE.
        wrtTLmod(ng)=.FALSE.
        CALL initial (ng)
        IF (exit_flag.ne.NoError) RETURN
      END DO
!
!  Save nonlinear initial conditions (currently in time index 1,
!  background) into record "Lini" of INI(ng)%name NetCDF file. The
!  record "Lbck" becomes the background state record and the record
!  "Lini" becomes current nonlinear initial conditions.
!
      DO ng=1,Ngrids
        INI(ng)%Rindex=1
        Fcount=INI(ng)%Fcount
        INI(ng)%Nrec(Fcount)=1
        CALL wrt_ini (ng, 1)
        IF (exit_flag.ne.NoError) RETURN
      END DO
!
!  Set nonlinear output history file as the initial basic state
!  trajectory.
!
      DO ng=1,Ngrids
        LdefHIS(ng)=.TRUE.
        LwrtHIS(ng)=.TRUE.
        WRITE (HIS(ng)%name,10) TRIM(FWD(ng)%base), outer
      END DO

#if defined BULK_FLUXES && defined NL_BULK_FLUXES
!
!  Set file name containing the nonlinear model bulk fluxes to be read
!  and processed by other algorithms.
!
      DO ng=1,Ngrids
        BLK(ng)%name=HIS(ng)%name
      END DO
#endif
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Model-error covariance normalization and stardard deviation factors.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
!  Compute or read in the error correlation normalization factors.
!  If computing, write out factors to NetCDF. This is an expensive
!  computation that needs to be computed only once for a particular
!  application grid and decorrelation scales.
!
      DO ng=1,Ngrids
        IF (ANY(LwrtNRM(:,ng))) THEN
          CALL def_norm (ng, iNLM, 1)
          IF (exit_flag.ne.NoError) RETURN

          IF (NSA.eq.2) THEN
            CALL def_norm (ng, iNLM, 2)
          IF (exit_flag.ne.NoError) RETURN
          END IF

#ifdef ADJUST_BOUNDARY
          CALL def_norm (ng, iNLM, 3)
          IF (exit_flag.ne.NoError) RETURN
#endif

#if defined ADJUST_WSTRESS || defined ADJUST_STFLUX
          CALL def_norm (ng, iNLM, 4)
          IF (exit_flag.ne.NoError) RETURN
#endif

!$OMP PARALLEL DO PRIVATE(thread,subs,tile) SHARED(numthreads)
          DO thread=0,numthreads-1
            subs=NtileX(ng)*NtileE(ng)/numthreads
            DO tile=subs*thread,subs*(thread+1)-1
              CALL normalization (ng, TILE, 2)
            END DO
          END DO
!$OMP END PARALLEL DO
          LdefNRM(1:4,ng)=.FALSE.
          LwrtNRM(1:4,ng)=.FALSE.
        ELSE
          NRMrec=1
          CALL get_state (ng, 5, 5, NRM(1,ng)%name, NRMrec, 1)
          IF (exit_flag.ne.NoError) RETURN

          IF (NSA.eq.2) THEN
            CALL get_state (ng, 5, 5, NRM(2,ng)%name, NRMrec, 2)
            IF (exit_flag.ne.NoError) RETURN
          END IF

#ifdef ADJUST_BOUNDARY
          CALL get_state (ng, 10, 10, NRM(3,ng)%name, NRMrec, 1)
          IF (exit_flag.ne.NoError) RETURN
#endif

#if defined ADJUST_WSTRESS || defined ADJUST_STFLUX
          CALL get_state (ng, 11, 11, NRM(4,ng)%name, NRMrec, 1)
          IF (exit_flag.ne.NoError) RETURN
#endif
        END IF
      END DO
!
!  Define tangent linear initial conditions file.
!
      DO ng=1,Ngrids
        LdefITL(ng)=.TRUE.
        CALL tl_def_ini (ng)
        LdefITL(ng)=.FALSE.
        IF (exit_flag.ne.NoError) RETURN
      END DO
!
!  Define TLM/RPM impulse forcing NetCDF file.
!
      DO ng=1,Ngrids
        LdefTLF(ng)=.TRUE.
        CALL def_impulse (ng)
        IF (exit_flag.ne.NoError) RETURN
      END DO
!
!  Define output 4DVAR NetCDF file containing all processed data
!  at observation locations.
!
      DO ng=1,Ngrids
        LdefMOD(ng)=.TRUE.
        CALL def_mod (ng)
        IF (exit_flag.ne.NoError) RETURN
      END DO
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Run nonlinear model and compute basic state trajectory.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
      DO ng=1,Ngrids
        IF (Master) THEN
          WRITE (stdout,20) 'NL', ng, ntstart(ng), ntend(ng)
        END IF
      END DO

#ifdef SOLVE3D
      CALL main3d (RunInterval)
#else
      CALL main2d (RunInterval)
#endif
      IF (exit_flag.ne.NoError) RETURN

      DO ng=1,Ngrids
        wrtNLmod(ng)=.FALSE.
      END DO
!
!  Set forward basic state NetCDF ID to nonlinear model trajectory to
!  avoid the inquiring stage.
!
      DO ng=1,Ngrids
        FWD(ng)%ncid=HIS(ng)%ncid
      END DO
!
!-----------------------------------------------------------------------
!  Solve the system:
!
!              (R_n + Cobs) * Beta_n = h_n
!
!              h_n = Xo - H * X_n
!
!  where R_n is the representer matrix, Cobs is the observation-error
!  covariance, Beta_n are the representer coefficients, h_n is the
!  misfit between observations (Xo) and model (H * X_n), and H is
!  the linearized observation operator. Here, _n denotes a sequence
!  of estimates.
!
!  The system does not need to be solved explicitly by inverting the
!  symmetric stabilized representer matrix, P_n:
!
!              P_n = R_n + Cobs
!
!  but by computing the action of P_n on any vector PSI, such that
!
!              P_n * PSI = R_n * PSI + Cobs * PSI
!
!  The representer matrix is not explicitly computed but evaluated by
!  one integration backward of the adjoint model and one integration
!  forward of the tangent linear model for any forcing vector PSI.
!
!  A preconditioned conjugate gradient algorithm is used to compute
!  an approximation PSI for Beta_n.
!
!-----------------------------------------------------------------------
!
      OUTER_LOOP1 : DO my_outer=1,Nouter
        outer=my_outer
        inner=0
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Run representer model and compute a "prior estimate" state
!  trajectory, X_n(t). Use linearized state trajectory (X_n-1) as
!  basic state.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
!  Set representer model basic state trajectory file to previous outer
!  loop file (outer-1). If outer=1, the basic state trajectory is the
!  nonlinear model.
!
        DO ng=1,Ngrids
          WRITE (FWD(ng)%name,10) TRIM(FWD(ng)%base), outer-1
        END DO
!
!  Set representer model output file name.  The strategy is to write
!  the representer solution at the beginning of each outer loop.
!
        DO ng=1,Ngrids
          LdefTLM(ng)=.TRUE.
          LwrtTLM(ng)=.TRUE.
          WRITE (TLM(ng)%name,10) TRIM(TLM(ng)%base), outer
        END DO
!
!  Activate switch to write the representer model at observation points.
!  Turn off writing into history file and turn off impulse forcing.
!
        DO ng=1,Ngrids
          wrtRPmod(ng)=.TRUE.
          SporadicImpulse(ng)=.FALSE.
          FrequentImpulse(ng)=.FALSE.
        END DO

#ifndef DATALESS_LOOPS
!
!  As in the nonlinear model, initialize always the representer model
!  here with the background or reference state (IRP(ng)%name, record
!  Rec1).
!
        DO ng=1,Ngrids
          IRP(ng)%Rindex=Rec1
          CALL rp_initial (ng)
          IF (exit_flag.ne.NoError) RETURN
        END DO
!
!  Run representer model using the nonlinear trajectory as a basic
!  state.  Compute model solution at observation points, H * X_n.
!
        DO ng=1,Ngrids
          IF (Master) THEN
            WRITE (stdout,20) 'RP', ng, ntstart(ng), ntend(ng)
          END IF
        END DO

# ifdef SOLVE3D
        CALL rp_main3d (RunInterval)
# else
        CALL rp_main2d (RunInterval)
# endif
        IF (exit_flag.ne.NoError) RETURN
!
!  Report data penalty function.
!
        DO ng=1,Ngrids
          IF (Master) THEN
            DO i=0,NstateVar(ng)
              IF (i.eq.0) THEN
                string='Total'
              ELSE
                string=Vname(1,idSvar(i))
              END IF
              IF (FOURDVAR(ng)%DataPenalty(i).ne.0.0_r8) THEN
                WRITE (stdout,30) outer, inner, 'RPM',                  &
     &                            FOURDVAR(ng)%DataPenalty(i),          &
     &                            TRIM(string)
              END IF
            END DO
          END IF
!
!  Write out initial data penalty function to NetCDF file.
!
          SourceFile='tl_w4dvar_ocean.h, ROMS_run'

          CALL netcdf_put_fvar (ng, iRPM, DAV(ng)%name,                 &
     &                          'RP_iDataPenalty',                      &
     &                          FOURDVAR(ng)%DataPenalty(0:),           &
     &                          (/1,outer/), (/NstateVar(ng)+1,1/),     &
     &                          ncid = DAV(ng)%ncid)
          IF (exit_flag.ne.NoError) RETURN
!
!  Clean penalty array before next run of RP model.
!
          FOURDVAR(ng)%DataPenalty=0.0_r8
        END DO
!
!  Turn off IO switches.
!
        DO ng=1,Ngrids
          LdefTLM(ng)=.FALSE.
          LwrtTLM(ng)=.FALSE.
          wrtRPmod(ng)=.FALSE.
        END DO
!
!  Clear tangent linear forcing arrays before entering inner-loop.
!  This is very important since these arrays are non-zero after
!  running the representer model and must be zero when running the
!  tangent linear model.
!
        DO ng=1,Ngrids
!$OMP PARALLEL DO PRIVATE(thread,subs,tile) SHARED(numthreads)
          DO thread=0,numthreads-1
# if defined _OPENMP || defined DISTRIBUTE
            subs=NtileX(ng)*NtileE(ng)/numthreads
# else
            subs=1
# endif
            DO tile=subs*thread,subs*(thread+1)-1
              CALL initialize_forces (ng, TILE, iTLM)
# ifdef ADJUST_BOUNDARY
              CALL initialize_boundary (ng, TILE, iTLM)
# endif
            END DO
          END DO
!$OMP END PARALLEL DO
        END DO

# if defined BALANCE_OPERATOR && defined ZETA_ELLIPTIC
!
!  Compute the reference zeta and biconjugate gradient arrays
!  required for the balance of free surface.
!
        IF (balance(isFsur)) THEN
          DO ng=1,Ngrids
          CALL get_state (ng, iNLM, 2, INI(ng)%name, Lini, Lini)
          IF (exit_flag.ne.NoError) RETURN
!
!$OMP PARALLEL DO PRIVATE(thread,subs,tile) SHARED(numthreads)
            DO thread=0,numthreads-1
              subs=NtileX(ng)*NtileE(ng)/numthreads
              DO tile=subs*thread,subs*(thread+1)-1
                CALL balance_ref (ng, TILE, Lini)
                CALL biconj (ng, TILE, iNLM, Lini)
              END DO
            END DO
!$OMP END PARALLEL DO
            wrtZetaRef(ng)=.TRUE.
          END DO
        END IF
# endif
!
        INNER_LOOP1 : DO my_inner=0,Ninner
          inner=my_inner
!
! Initialize conjugate gradient algorithm depending on hot start or
! outer loop index.
!
          IF (inner.eq.0) THEN
            Lcgini=.TRUE.
            DO ng=1,Ngrids
              CALL congrad (ng, iRPM, outer, inner, Ninner, Lcgini)
            END DO
          END IF
!
!  If initialization step, skip the inner-loop computations.
!
          Linner=.FALSE.
          IF ((inner.ne.0).or.(Nrun.ne.1)) THEN
            IF (((inner.eq.0).and.LhotStart).or.(inner.ne.0)) THEN
              Linner=.TRUE.
            END IF
          END IF
!
!  Start inner loop computations.
!
          INNER_COMPUTE1 : IF (Linner) THEN
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Integrate adjoint model forced with any vector PSI at the observation
!  locations and generate adjoint trajectory, Lambda_n(t).
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
!  Initialize the adjoint model from rest.
!
            DO ng=1,Ngrids
              CALL ad_initial (ng)
              IF (exit_flag.ne.NoError) RETURN
              wrtMisfit(ng)=.FALSE.
            END DO

# ifdef RPM_RELAXATION
!
!  Adjoint of representer relaxation is not applied during the
!  inner-loops.
!
            DO ng=1,Ngrids
              LweakRelax(ng)=.FALSE.
            END DO
# endif
!
!  Set adjoint history NetCDF parameters.  Define adjoint history
!  file only once to avoid opening too many files.
!
            DO ng=1,Ngrids
              WRTforce(ng)=.TRUE.
              IF (Nrun.gt.1) LdefADJ(ng)=.FALSE.
              Fcount=ADM(ng)%Fcount
              ADM(ng)%Nrec(Fcount)=0
              ADM(ng)%Rindex=0
            END DO
!
!  Time-step adjoint model backwards forced with current PSI vector.
!
            DO ng=1,Ngrids
              IF (Master) THEN
                WRITE (stdout,20) 'AD', ng, ntstart(ng), ntend(ng)
              END IF
            END DO

# ifdef SOLVE3D
            CALL ad_main3d (RunInterval)
# else
            CALL ad_main2d (RunInterval)
# endif
            IF (exit_flag.ne.NoError) RETURN
!
!  Write out last weak-constraint forcing (WRTforce is still .TRUE.)
!  record into the adjoint history file.  Note that the weak-constraint
!  forcing is delayed by nADJ time-steps.
!
            DO ng=1,Ngrids
              CALL ad_wrt_his (ng)
              IF (exit_flag.ne.NoError) RETURN
            END DO
!
!  Write out adjoint initial condition record into the adjoint
!  history file.
!
            DO ng=1,Ngrids
              WRTforce(ng)=.FALSE.
              CALL ad_wrt_his (ng)
              IF (exit_flag.ne.NoError) RETURN
            END DO
!
!  Convolve adjoint trajectory with error covariances.
!
            Lposterior=.FALSE.
            CALL error_covariance (iTLM, driver, outer, inner,          &
     &                             Lbck, Lini, Lold, Lnew,              &
     &                             Rec1, Rec2, Lposterior)
            IF (exit_flag.ne.NoError) RETURN
!
!  Convert the current adjoint solution in ADM(ng)%name to impulse
!  forcing. Write out impulse forcing into TLF(ng)%name NetCDF file.
!  To facilitate the forcing to the TLM and RPM, the forcing is
!  processed and written in increasing time coordinates (recall that
!  the adjoint solution in ADM(ng)%name is backwards in time).
!
            IF (Master) THEN
              WRITE (stdout,40) outer, inner
            END IF
            DO ng=1,Ngrids
              TLF(ng)%Rindex=0
# ifdef DISTRIBUTE
              tile=MyRank
# else
              tile=-1
# endif
              CALL wrt_impulse (ng, tile, iADM, ADM(ng)%name)
              IF (exit_flag.ne.NoError) RETURN
            END DO
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Integrate tangent linear model forced by the convolved adjoint
!  trajectory (impulse forcing) to compute R_n * PSI at observation
!  points.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
            DO ng=1,Ngrids
              TLM(ng)%name=TRIM(TLM(ng)%base)//'.nc'
              wrtNLmod(ng)=.FALSE.
              wrtTLmod(ng)=.TRUE.
            END DO
!
!  If weak constraint, the impulses are time-interpolated at each
!  time-steps.
!
            DO ng=1,Ngrids
              IF (FrcRec(ng).gt.3) THEN
                FrequentImpulse(ng)=.TRUE.
              END IF
            END DO
!
!  Initialize tangent linear model from ITL(ng)%name, record Rec1.
!
            DO ng=1,Ngrids
              ITL(ng)%Rindex=Rec1
              CALL tl_initial (ng)
              IF (exit_flag.ne.NoError) RETURN
            END DO
!
!  Activate switch to write out initial misfit between model and
!  observations.
!
            IF ((outer.eq.1).and.(inner.eq.1)) THEN
              DO ng=1,Ngrids
                wrtMisfit(ng)=.TRUE.
              END DO
            END IF
!
!  Set tangent linear history NetCDF parameters.  Define tangent linear
!  history file at the beggining of each inner loop  to avoid opening
!  too many NetCDF files.
!
            DO ng=1,Ngrids
              IF (inner.gt.1) LdefTLM(ng)=.FALSE.
              Fcount=TLM(ng)%Fcount
              TLM(ng)%Nrec(Fcount)=0
              TLM(ng)%Rindex=0
            END DO
!
!  Run tangent linear model forward and force with convolved adjoint
!  trajectory impulses. Compute R_n * PSI at observation points which
!  are used in the conjugate gradient algorithm.
!
            DO ng=1,Ngrids
              IF (Master) THEN
                WRITE (stdout,20) 'TL', ng, ntstart(ng), ntend(ng)
              END IF
            END DO

# ifdef SOLVE3D
            CALL tl_main3d (RunInterval)
# else
            CALL tl_main2d (RunInterval)
# endif
            IF (exit_flag.ne.NoError) RETURN

            DO ng=1,Ngrids
              wrtNLmod(ng)=.FALSE.
              wrtTLmod(ng)=.FALSE.
            END DO
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Use conjugate gradient algorithm to find a better approximation
!  PSI to representer coefficients Beta_n.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
            Nrun=Nrun+1
            DO ng=1,Ngrids
              Lcgini=.FALSE.
              CALL congrad (ng, iRPM, outer, inner, Ninner, Lcgini)
              IF (exit_flag.ne.NoError) RETURN
            END DO

          END IF INNER_COMPUTE1

        END DO INNER_LOOP1
!
!  Close tangent linear NetCDF file.
!
        SourceFile='tl_w4dvar_ocean.h, ROMS_run'
        DO ng=1,Ngrids
          CALL netcdf_close (ng, iTLM, TLM(ng)%ncid)
          IF (exit_flag.ne.NoError) RETURN
        END DO
!
!-----------------------------------------------------------------------
!  Once that the representer coefficients, Beta_n, have been
!  approximated with sufficient accuracy, compute estimates of
!  Lambda_n and Xhat_n by carrying out one backward intergration
!  of the adjoint model and one forward itegration of the representer
!  model.
!-----------------------------------------------------------------------
!
!  Initialize the adjoint model always from rest.
!
        DO ng=1,Ngrids
          CALL ad_initial (ng)
          IF (exit_flag.ne.NoError) RETURN
        END DO

# ifdef RPM_RELAXATION
!
!  Adjoint of representer relaxation is applied during the
!  outer-loops.
!
        DO ng=1,Ngrids
          LweakRelax(ng)=.TRUE.
        END DO
# endif
!
!  Set adjoint history NetCDF parameters.  Define adjoint history
!  file one to avoid opening to many files.
!
        DO ng=1,Ngrids
          WRTforce(ng)=.TRUE.
          IF (Nrun.gt.1) LdefADJ(ng)=.FALSE.
          Fcount=ADM(ng)%Fcount
          ADM(ng)%Nrec(Fcount)=0
          ADM(ng)%Rindex=0
        END DO
!
!  Time-step adjoint model backwards forced with estimated representer
!  coefficients, Beta_n.
!
        DO ng=1,Ngrids
          IF (Master) THEN
            WRITE (stdout,20) 'AD', ng, ntstart(ng), ntend(ng)
          END IF
        END DO

# ifdef SOLVE3D
        CALL ad_main3d (RunInterval)
# else
        CALL ad_main2d (RunInterval)
# endif
        IF (exit_flag.ne.NoError) RETURN
!
!  Write out last weak-constraint forcing (WRTforce is still .TRUE.)
!  record into the adjoint history file.  Note that the weak-constraint
!  forcing is delayed by nADJ time-steps.
!
        DO ng=1,Ngrids
          CALL ad_wrt_his (ng)
          IF (exit_flag.ne.NoError) RETURN
        END DO
!
!  Write out adjoint initial condition record into the adjoint
!  history file.
!
        DO ng=1,Ngrids
          WRTforce(ng)=.FALSE.
          CALL ad_wrt_his (ng)
          IF (exit_flag.ne.NoError) RETURN
        END DO
!
!  Convolve adjoint trajectory with error covariances.
!
        Lposterior=.FALSE.
        CALL error_covariance (iRPM, driver, outer, inner,              &
     &                         Lbck, Lini, Lold, Lnew,                  &
     &                         Rec1, Rec2, Lposterior)
        IF (exit_flag.ne.NoError) RETURN
!
!  Convert the current adjoint solution in ADM(ng)%name to impulse
!  forcing. Write out impulse forcing into TLF(ng)%name NetCDF file.
!  To facilitate the forcing to the TLM and RPM, the forcing is
!  processed and written in increasing time coordinates (recall that
!  the adjoint solution in ADM(ng)%name is backwards in time).
!
        IF (Master) THEN
          WRITE (stdout,40) outer, inner
        END IF
        DO ng=1,Ngrids
          TLF(ng)%Rindex=0
# ifdef DISTRIBUTE
          tile=MyRank
# else
          tile=-1
# endif
          CALL wrt_impulse (ng, tile, iADM, ADM(ng)%name)
          IF (exit_flag.ne.NoError) RETURN
        END DO

#endif /* !DATALESS_LOOPS */
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Run representer model and compute a "new estimate" of the state
!  trajectory, X_n(t).
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
!  Set new basic state trajectory for next outer loop.
!
        DO ng=1,Ngrids
          LdefTLM(ng)=.TRUE.
          LwrtTLM(ng)=.TRUE.
          wrtNLmod(ng)=.FALSE.
          wrtTLmod(ng)=.TRUE.
          wrtRPmod(ng)=.TRUE.
          WRITE (TLM(ng)%name,10) TRIM(FWD(ng)%base), outer
        END DO
!
!  If weak constraint, the impulses are time-interpolated at each
!  time-steps.
!
        DO ng=1,Ngrids
          IF (FrcRec(ng).gt.3) THEN
            FrequentImpulse(ng)=.TRUE.
          END IF
        END DO
!
!  Initialize representer model IRP(ng)%name file, record Rec2.
!
        DO ng=1,Ngrids
#ifdef DATALESS_LOOPS
          IRP(ng)%Rindex=Rec1
#else
          IRP(ng)%Rindex=Rec2
#endif
          CALL rp_initial (ng)
          IF (exit_flag.ne.NoError) RETURN
        END DO
!
!  Activate switch to write out final misfit between model and
!  observations.
!
        IF (outer.eq.Nouter) THEN
          DO ng=1,Ngrids
            wrtMisfit(ng)=.TRUE.
          END DO
        END IF
!
!  Run representer model using previous linearized trajectory, X_n-1, as
!  basic state and forced with convolved adjoint trajectory impulses.
!
        DO ng=1,Ngrids
          IF (Master) THEN
            WRITE (stdout,20) 'RP', ng, ntstart(ng), ntend(ng)
          END IF
        END DO

#ifdef SOLVE3D
        CALL rp_main3d (RunInterval)
#else
        CALL rp_main2d (RunInterval)
#endif
        IF (exit_flag.ne.NoError) RETURN
!
!  Report data penalty function.
!
        DO ng=1,Ngrids
          IF (Master) THEN
            DO i=0,NstateVar(ng)
              IF (i.eq.0) THEN
                string='Total'
              ELSE
                string=Vname(1,idSvar(i))
              END IF
              IF (FOURDVAR(ng)%DataPenalty(i).ne.0.0_r8) THEN
                WRITE (stdout,30) outer, inner, 'RPM',                  &
     &                            FOURDVAR(ng)%DataPenalty(i),          &
     &                            TRIM(string)
#ifdef DATALESS_LOOPS
                WRITE (stdout,30) outer, inner, 'NLM',                  &
     &                            FOURDVAR(ng)%NLPenalty(i),            &
     &                            TRIM(string)
#endif
              END IF
            END DO
          END IF
        END DO
!
!  Write out final data penalty function to NetCDF file.
!
        SourceFile='tl_w4dvar_ocean.h, ROMS_run'
        DO ng=1,Ngrids
          CALL netcdf_put_fvar (ng, iRPM, DAV(ng)%name,                 &
     &                          'RP_fDataPenalty',                      &
     &                          FOURDVAR(ng)%DataPenalty(0:),           &
     &                          (/1,outer/), (/NstateVar(ng)+1,1/),     &
     &                          ncid = DAV(ng)%ncid)
          IF (exit_flag.ne.NoError) RETURN
        END DO
!
!  Clean array before next run of RP model.
!
        DO ng=1,Ngrids
          FOURDVAR(ng)%DataPenalty=0.0_r8
#ifdef DATALESS_LOOPS
          FOURDVAR(ng)%NLPenalty=0.0_r8
#endif
          wrtNLmod(ng)=.FALSE.
          wrtTLmod(ng)=.FALSE.
        END DO
!
!  Close current forward NetCDF file.
!
        DO ng=1,Ngrids
          CALL netcdf_close (ng, iRPM, FWD(ng)%ncid)
          IF (exit_flag.ne.NoError) RETURN
        END DO

      END DO OUTER_LOOP1
!
!  Done.  Set history file ID to closed state since we manipulated
!  its indices with the forward file ID which was closed above.
!
      DO ng=1,Ngrids
        HIS(ng)%ncid=-1
      END DO
!!
!! Compute and report model-observation comparison statistics.
!!
!!    DO ng=1,Ngrids
!!      CALL stats_modobs (ng)
!!    END DO
!
!~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
!~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
!  Tangent linear of the data assimilation system.
!~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
!~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
!
!  Read in observations.
!
      SourceFile='tl_w4dvar_ocean.h, ROMS_run'
      DO ng=1,Ngrids
        Mstr=NstrObs(ng)
        CALL netcdf_get_fvar (ng, iTLM, OBS(ng)%name,                   &
     &                        'obs_value',                              &
     &                        tl_ObsVal(Mstr:),                         &
     &                        start = (/NstrObs(ng)/),                  &
     &                        total = (/Nobs(ng)/))
      END DO
!
!  Now run the tangent linear of W4DVAR.
!
!  Reset value of "Nrun" and clear "TLmodVal".
!
      Nrun=1
      TLmodVal=0.0_r8
!
!  Start outer loop.
!
      OUTER_LOOP2 : DO my_outer=1,Nouter
        outer=my_outer
        inner=0
!
!  Set basic state trajectory (X_n-1) file to previous outer loop file
!  (outer-1).
!
        DO ng=1,Ngrids
          WRITE (FWD(ng)%name,10) TRIM(FWD(ng)%base), outer-1
        END DO
!
!  Clear tangent linear forcing arrays before entering inner-loop.
!  This is very important since these arrays are non-zero after
!  running the representer model and must be zero when running the
!  tangent linear model.
!
        DO ng=1,Ngrids
!$OMP PARALLEL DO PRIVATE(thread,subs,tile) SHARED(numthreads)
          DO thread=0,numthreads-1
#if defined _OPENMP || defined DISTRIBUTE
            subs=NtileX(ng)*NtileE(ng)/numthreads
#else
            subs=1
#endif
            DO tile=subs*thread,subs*(thread+1)-1
              CALL initialize_forces (ng, TILE, iTLM)
            END DO
          END DO
!$OMP END PARALLEL DO
        END DO
!
        INNER_LOOP2 : DO my_inner=0,Ninner
          inner=my_inner

          IF (Master) THEN
            WRITE (stdout,50) 'Tangent linear of',                      &
     &                        uppercase('w4dvar'), outer, inner
          END IF
!
! Initialize tangent linear conjugate gradient algorithm depending on
! hot start or outer loop index.
!
          IF (inner.eq.0) THEN
            Lcgini=.TRUE.
            DO ng=1,Ngrids
              CALL tl_congrad (ng, iRPM, outer, inner, Ninner, Lcgini)
            END DO
          END IF
!
!  If initialization step, skip the inner-loop computations.
!
          Linner=.FALSE.
          IF ((inner.ne.0).or.(Nrun.ne.1)) THEN
            IF (((inner.eq.0).and.LhotStart).or.(inner.ne.0)) THEN
              Linner=.TRUE.
            END IF
          END IF
!
!  Start inner loop computations.
!
          INNER_COMPUTE2 : IF (Linner) THEN
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Integrate adjoint model forced with any vector PSI at the observation
!  locations and generate adjoint trajectory, Lambda_n(t).
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
!  Initialize the adjoint model from rest.
!
            DO ng=1,Ngrids
              CALL ad_initial (ng)
              IF (exit_flag.ne.NoError) RETURN
              wrtMisfit(ng)=.FALSE.
            END DO
!
!  Adjoint of representer relaxation is not applied during the
!  inner-loops.
!
            DO ng=1,Ngrids
              LweakRelax(ng)=.FALSE.
            END DO
!
!  Set adjoint history NetCDF parameters.  Define adjoint history
!  file only once to avoid opening too many files.
!
            DO ng=1,Ngrids
              WRTforce(ng)=.TRUE.
              IF (Nrun.gt.1) LdefADJ(ng)=.FALSE.
              Fcount=ADM(ng)%Fcount
              ADM(ng)%Nrec(Fcount)=0
              ADM(ng)%Rindex=0
            END DO
!
!  Time-step adjoint model backwards forced with current PSI vector.
!
            DO ng=1,Ngrids
              IF (Master) THEN
                WRITE (stdout,20) 'AD', ng, ntstart(ng), ntend(ng)
              END IF
            END DO

#ifdef SOLVE3D
            CALL ad_main3d (RunInterval)
#else
            CALL ad_main2d (RunInterval)
#endif
            IF (exit_flag.ne.NoError) RETURN
!
!  Write out last weak-constraint forcing (WRTforce is still .TRUE.)
!  record into the adjoint history file.  Note that the weak-constraint
!  forcing is delayed by nADJ time-steps
!
            DO ng=1,Ngrids
              CALL ad_wrt_his (ng)
              IF (exit_flag.ne.NoError) RETURN
            END DO
!
!  Write out adjoint initial condition record into the adjoint
!  history file.
!
            DO ng=1,Ngrids
              WRTforce(ng)=.FALSE.
              CALL ad_wrt_his (ng)
              IF (exit_flag.ne.NoError) RETURN
            END DO
!
!  Convolve adjoint trajectory with error covariances.
!
            Lposterior=.FALSE.
            CALL error_covariance (iTLM, driver, outer, inner,          &
     &                             Lbck, Lini, Lold, Lnew,              &
     &                             Rec1, Rec2, Lposterior)
            IF (exit_flag.ne.NoError) RETURN
!
!  Convert the current adjoint solution in ADM(ng)%name to impulse
!  forcing. Write out impulse forcing into TLF(ng)%name NetCDF file.
!  To facilitate the forcing to the TLM and RPM, the forcing is
!  processed and written in increasing time coordinates (recall that
!  the adjoint solution in ADM(ng)%name is backwards in time).
!
            IF (Master) THEN
              WRITE (stdout,40) outer, inner
            END IF
            DO ng=1,Ngrids
              TLF(ng)%Rindex=0
#ifdef DISTRIBUTE
              tile=MyRank
#else
              tile=-1
#endif
              CALL wrt_impulse (ng, tile, iADM, ADM(ng)%name)
              IF (exit_flag.ne.NoError) RETURN
            END DO
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Integrate tangent linear model forced by the convolved adjoint
!  trajectory (impulse forcing) to compute R_n * PSI at observation
!  points.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
            DO ng=1,Ngrids
              TLM(ng)%name=TRIM(TLM(ng)%base)//'.nc'
              wrtNLmod(ng)=.FALSE.
              wrtTLmod(ng)=.TRUE.
            END DO
!
!  If weak constraint, the impulses are time-interpolated at each
!  time-steps.
!
            DO ng=1,Ngrids
              IF (FrcRec(ng).gt.3) THEN
                FrequentImpulse(ng)=.TRUE.
              END IF
            END DO
!
!  Initialize tangent linear model from ITL(ng)%name, record Rec1.
!
            DO ng=1,Ngrids
              ITL(ng)%Rindex=Rec1
              CALL tl_initial (ng)
              IF (exit_flag.ne.NoError) RETURN
            END DO
!
!  Activate switch to write out initial misfit between model and
!  observations.
!
            IF ((outer.eq.1).and.(inner.eq.1)) THEN
              DO ng=1,Ngrids
                wrtMisfit(ng)=.TRUE.
              END DO
            END IF
!
!  Set tangent linear history NetCDF parameters.  Define tangent linear
!  history file at the beggining of each inner loop  to avoid opening
!  too many NetCDF files.
!
            DO ng=1,Ngrids
              IF (inner.gt.1) LdefTLM(ng)=.FALSE.
              Fcount=TLM(ng)%Fcount
              TLM(ng)%Nrec(Fcount)=0
              TLM(ng)%Rindex=0
            END DO
!
!  Run tangent linear model forward and force with convolved adjoint
!  trajectory impulses. Compute R_n * PSI at observation points which
!  are used in the conjugate gradient algorithm.
!
            DO ng=1,Ngrids
              IF (Master) THEN
                WRITE (stdout,20) 'TL', ng, ntstart(ng), ntend(ng)
              END IF
            END DO

#ifdef SOLVE3D
            CALL tl_main3d (RunInterval)
#else
            CALL tl_main2d (RunInterval)
#endif

            DO ng=1,Ngrids
              wrtNLmod(ng)=.FALSE.
              wrtTLmod(ng)=.FALSE.
            END DO
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Use conjugate gradient algorithm to find a better approximation
!  PSI to representer coefficients Beta_n. Exit inner loop if
!  convergence is achieved.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
            Nrun=Nrun+1
            Lcgini=.FALSE.
            DO ng=1,Ngrids
              CALL tl_congrad (ng, iRPM, outer, inner, Ninner, Lcgini)
              IF (exit_flag.ne.NoError) RETURN
            END DO

          END IF INNER_COMPUTE2

        END DO INNER_LOOP2
!
!-----------------------------------------------------------------------
!  Once that the representer coefficients, Beta_n, have been
!  approximated with sufficient accuracy, compute estimates of
!  Lambda_n and Xhat_n by carrying out one backward intergration
!  of the adjoint model and one forward itegration of the representer
!  model.
!-----------------------------------------------------------------------
!
!  Initialize the adjoint model always from rest.
!
        DO ng=1,Ngrids
          CALL ad_initial (ng)
          IF (exit_flag.ne.NoError) RETURN
        END DO
!
!  Adjoint of representer relaxation is applied during the
!  outer-loops.
!
        DO ng=1,Ngrids
          LweakRelax(ng)=.TRUE.
        END DO
!
!  Set adjoint history NetCDF parameters.  Define adjoint history
!  file one to avoid opening to many files.
!
        DO ng=1,Ngrids
          WRTforce(ng)=.TRUE.
          IF (Nrun.gt.1) LdefADJ(ng)=.FALSE.
          Fcount=ADM(ng)%Fcount
          ADM(ng)%Nrec(Fcount)=0
          ADM(ng)%Rindex=0
        END DO
!
!  Time-step adjoint model backwards forced with estimated representer
!  coefficients, Beta_n.
!
        DO ng=1,Ngrids
          IF (Master) THEN
            WRITE (stdout,20) 'AD', ng, ntstart(ng), ntend(ng)
          END IF
        END DO

#ifdef SOLVE3D
        CALL ad_main3d (RunInterval)
#else
        CALL ad_main2d (RunInterval)
#endif
        IF (exit_flag.ne.NoError) RETURN
!
!  Write out last weak-constraint forcing (WRTforce is still .TRUE.)
!  record into the adjoint history file.  Note that the weak-constraint
!  forcing is delayed by nADJ time-steps
!
        DO ng=1,Ngrids
          CALL ad_wrt_his (ng)
          IF (exit_flag.ne.NoError) RETURN
        END DO
!
!  Write out adjoint initial condition record into the adjoint
!  history file.
!
        DO ng=1,Ngrids
          WRTforce(ng)=.FALSE.
          CALL ad_wrt_his (ng)
          IF (exit_flag.ne.NoError) RETURN
        END DO
!
!  Convolve adjoint trajectory with error covariances.
!
        Lposterior=.FALSE.
        CALL error_covariance (iTLM, driver, outer, inner,              &
     &                         Lbck, Lini, Lold, Lnew,                  &
     &                         Rec1, Rec2, Lposterior)
        IF (exit_flag.ne.NoError) RETURN
!
!  Convert the current adjoint solution in ADM(ng)%name to impulse
!  forcing. Write out impulse forcing into TLF(ng)%name NetCDF file.
!  To facilitate the forcing to the TLM and RPM, the forcing is
!  processed and written in increasing time coordinates (recall that
!  the adjoint solution in ADM(ng)%name is backwards in time).
!
        IF (Master) THEN
          WRITE (stdout,40) outer, inner
        END IF
        DO ng=1,Ngrids
          TLF(ng)%Rindex=0
#ifdef DISTRIBUTE
          tile=MyRank
#else
          tile=-1
#endif
          CALL wrt_impulse (ng, tile, iADM, ADM(ng)%name)
          IF (exit_flag.ne.NoError) RETURN
        END DO
!
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!  Integrate tangent linear model forced by the convolved adjoint
!  trajectory (impulse forcing) to compute R_n * PSI at observation
!  points.
!:::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::::
!
        DO ng=1,Ngrids
          WRITE (FWD(ng)%name,10) TRIM(FWD(ng)%base), outer-1
          WRITE (TLM(ng)%name,10) TRIM(TLM(ng)%base), outer+1
        END DO
!
!  Initialize tangent linear model from initial impulse which is now
!  stored in file ITL(ng)%name.
!
        DO ng=1,Ngrids
          wrtNLmod(ng)=.FALSE.
          wrtTLmod(ng)=.TRUE.
          LdefTLM(ng)=.TRUE.
          LwrtTLM(ng)=.TRUE.
        END DO
!
!  If weak constraint, the impulses are time-interpolated at each
!  time-steps.
!
        DO ng=1,Ngrids
          IF (FrcRec(ng).gt.3) THEN
            FrequentImpulse(ng)=.TRUE.
          END IF
        END DO
!
!  Initialize tangent linear model from ITL(ng)%name, record Rec1.
!
        DO ng=1,Ngrids
          ITL(ng)%Rindex=Rec1
          CALL tl_initial (ng)
          IF (exit_flag.ne.NoError) RETURN
        END DO
!
!  Set tangent linear history NetCDF parameters.  Define tangent linear
!  history file at the beggining of each inner loop  to avoid opening
!  too many NetCDF files.
!
        DO ng=1,Ngrids
! AMM     IF (inner.gt.1) LdefTLM(ng)=.FALSE.
          Fcount=TLM(ng)%Fcount
          TLM(ng)%Nrec(Fcount)=0
          TLM(ng)%Rindex=0
        END DO
!
!  Run tangent linear model forward and force with convolved adjoint
!  trajectory impulses. Compute (HMBM'H')_n * PSI at observation points
!  which are used in the conjugate gradient algorithm.
!
        DO ng=1,Ngrids
          IF (Master) THEN
            WRITE (stdout,20) 'TL', ng, ntstart(ng), ntend(ng)
          END IF
        END DO

#ifdef SOLVE3D
        CALL tl_main3d (RunInterval)
#else
        CALL tl_main2d (RunInterval)
#endif
        IF (exit_flag.ne.NoError) RETURN

        DO ng=1,Ngrids
          wrtNLmod(ng)=.FALSE.
          wrtTLmod(ng)=.FALSE.
        END DO
!
!  Close tangent linear NetCDF file.
!
        SourceFile='tl_w4dvar_ocean.h, ROMS_run'
        DO ng=1,Ngrids
          CALL netcdf_close (ng, iTLM, TLM(ng)%ncid)
          TLM(ng)%ncid=-1
        END DO
!
!  Close current forward NetCDF file.
!
        SourceFile='tl_w4dvar_ocean.h, ROMS_run'
        DO ng=1,Ngrids
          CALL netcdf_close (ng, iNLM, FWD(ng)%ncid)
          FWD(ng)%ncid=-1
        END DO
!
      END DO OUTER_LOOP2
!
 10   FORMAT (a,'_',i3.3,'.nc')
 20   FORMAT (/,1x,a,1x,'ROMS/TOMS: started time-stepping:',            &
     &        ' (Grid: ',i2.2,' TimeSteps: ',i8.8,' - ',i8.8,')',/)
 30   FORMAT (' (',i3.3,',',i3.3,'): ',a,' data penalty, Jdata = ',     &
     &        1p,e16.10,0p,t68,a)
 40   FORMAT (/,' Converting Convolved Adjoint Trajectory to',          &
     &          ' Impulses: Outer = ',i3.3,' Inner = ',i3.3,/)
 50   FORMAT (/,'ROMS/TOMS: ',a,1x,a,', Outer = ',i3.3,                 &
     &          ' Inner = ',i3.3,/)

      RETURN
      END SUBROUTINE ROMS_run

      SUBROUTINE ROMS_finalize
!
!=======================================================================
!                                                                      !
!  This routine terminates ROMS/TOMS nonlinear, tangent linear, and    !
!  adjoint models execution.                                           !
!                                                                      !
!=======================================================================
!
      USE mod_param
      USE mod_parallel
      USE mod_iounits
      USE mod_ncparam
      USE mod_scalars
!
!  Local variable declarations.
!
      integer :: Fcount, ng, thread
!
!-----------------------------------------------------------------------
!  If blowing-up, save latest model state into RESTART NetCDF file.
!-----------------------------------------------------------------------
!
!  If cycling restart records, write solution into record 3.
!
      IF (exit_flag.eq.1) THEN
        DO ng=1,Ngrids
          IF (LwrtRST(ng)) THEN
            IF (Master) WRITE (stdout,10)
 10         FORMAT (/,' Blowing-up: Saving latest model state into ',   &
     &                ' RESTART file',/)
            Fcount=RST(ng)%Fcount
            IF (LcycleRST(ng).and.(RST(ng)%Nrec(Fcount).ge.2)) THEN
              RST(ng)%Rindex=2
              LcycleRST(ng)=.FALSE.
            END IF
            blowup=exit_flag
            exit_flag=NoError
            CALL wrt_rst (ng)
          END IF
        END DO
      END IF
!
!-----------------------------------------------------------------------
!  Stop model and time profiling clocks.  Close output NetCDF files.
!-----------------------------------------------------------------------
!
!  Stop time clocks.
!
      IF (Master) THEN
        WRITE (stdout,20)
 20     FORMAT (/,'Elapsed CPU time (seconds):',/)
      END IF

      DO ng=1,Ngrids
!$OMP PARALLEL DO PRIVATE(thread) SHARED(numthreads)
        DO thread=0,numthreads-1
          CALL wclock_off (ng, iNLM, 0)
        END DO
!$OMP END PARALLEL DO
      END DO
!
!  Close IO files.
!
      CALL close_out

      RETURN
      END SUBROUTINE ROMS_finalize

      END MODULE ocean_control_mod
