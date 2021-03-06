// rbOOmit: An implementation of the Certified Reduced Basis method.
// Copyright (C) 2009, 2010 David J. Knezevic

// This file is part of rbOOmit.

// rbOOmit is free software; you can redistribute it and/or
// modify it under the terms of the GNU Lesser General Public
// License as published by the Free Software Foundation; either
// version 2.1 of the License, or (at your option) any later version.

// rbOOmit is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// Lesser General Public License for more details.

// You should have received a copy of the GNU Lesser General Public
// License along with this library; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA


/**
 * Specialization of DerivedRBConstruction for Base = RBConstruction
 * which is typedef'd to SteadyDerivedRBConstruction
 */

#include "libmesh/derived_rb_construction.h"
#include "libmesh/derived_rb_evaluation.h"

#include "libmesh/libmesh_logging.h"
#include "libmesh/equation_systems.h"
#include "libmesh/mesh_base.h"
#include "libmesh/exodusII_io.h"

namespace libMesh
{

template <>
UniquePtr<RBEvaluation> DerivedRBConstruction<RBConstruction>::build_rb_evaluation(const Parallel::Communicator &comm_in)
{
  return UniquePtr<RBEvaluation>
    ( new DerivedRBEvaluation<RBEvaluation>(comm_in) );
}

template <>
DenseVector<Number> DerivedRBConstruction<RBConstruction>::get_derived_basis_function(unsigned int i)
{
  DerivedRBEvaluation<RBEvaluation>& der_rb_eval =
    cast_ref<DerivedRBEvaluation<RBEvaluation>&>(get_rb_evaluation());

  return der_rb_eval.derived_basis_functions[i];
}

template <>
Real DerivedRBConstruction<RBConstruction>::truth_solve(int plot_solution)
{
  START_LOG("truth_solve()", "DerivedRBConstruction");

  EquationSystems& es = this->get_equation_systems();
  RBConstruction& uber_system = es.get_system<RBConstruction>(uber_system_name);

  set_uber_current_parameters();

  uber_system.get_rb_evaluation().set_parameters(uber_system.get_parameters());
  uber_system.get_rb_evaluation().rb_solve(uber_system.get_rb_evaluation().get_n_basis_functions());

  if(plot_solution > 0)
    {
      uber_system.load_rb_solution();
      *solution = *(uber_system.solution);

#ifdef LIBMESH_HAVE_EXODUS_API
      ExodusII_IO(get_mesh()).write_equation_systems ("unter_uber_truth.e",
                                                      this->get_equation_systems());
#endif
    }

  STOP_LOG("truth_solve()", "DerivedRBConstruction");

  // Don't bother returning the norm of the uber solution
  return 0.;
}

template <>
void DerivedRBConstruction<RBConstruction>::enrich_RB_space()
{
  START_LOG("enrich_RB_space()", "DerivedRBConstruction");

  EquationSystems& es = this->get_equation_systems();
  RBConstruction& uber_system = es.get_system<RBConstruction>(uber_system_name);
  const unsigned int uber_size = uber_system.get_rb_evaluation().get_n_basis_functions();

  DenseVector<Number> new_bf = uber_system.get_rb_evaluation().RB_solution;

  // Need to cast the RBEvaluation object
  DerivedRBEvaluation<RBEvaluation>& der_rb_eval =
    cast_ref<DerivedRBEvaluation<RBEvaluation>&>(get_rb_evaluation());

  // compute Gram-Schmidt orthogonalization
  DenseVector<Number> proj_sum(uber_size);
  for(unsigned int index=0; index<get_rb_evaluation().get_n_basis_functions(); index++)
    {
      // orthogonalize using the Identity matrix as the inner product,
      // since the uber basis functions should be orthogonal already
      // (i.e. neglect possible rounding errors in uber orthogonalization)
      Number scalar = new_bf.dot(der_rb_eval.derived_basis_functions[index]);
      proj_sum.add(scalar, der_rb_eval.derived_basis_functions[index]);
    }
  new_bf -= proj_sum;
  new_bf.scale(1./new_bf.l2_norm());

  // load the new basis function into the basis_functions vector.
  der_rb_eval.derived_basis_functions.push_back( new_bf );

  STOP_LOG("enrich_RB_space()", "DerivedRBConstruction");
}


template <>
void DerivedRBConstruction<RBConstruction>::update_RB_system_matrices()
{
  START_LOG("update_RB_system_matrices()", "DerivedRBConstruction");

  DerivedRBEvaluation<RBEvaluation>& der_rb_eval =
    cast_ref<DerivedRBEvaluation<RBEvaluation>&>(get_rb_evaluation());

  EquationSystems& es = this->get_equation_systems();
  RBConstruction& uber_system = es.get_system<RBConstruction>(uber_system_name);

  unsigned int derived_RB_size = get_rb_evaluation().get_n_basis_functions();
  unsigned int uber_RB_size    = uber_system.get_rb_evaluation().get_n_basis_functions();

  const unsigned int Q_a = get_rb_theta_expansion().get_n_A_terms();
  const unsigned int Q_f = get_rb_theta_expansion().get_n_F_terms();

  DenseVector<Number> temp_vector;
  for(unsigned int q_f=0; q_f<Q_f; q_f++)
    {
      for(unsigned int i=(derived_RB_size-delta_N); i<derived_RB_size; i++)
        {
          uber_system.get_rb_evaluation().RB_Fq_vector[q_f].get_principal_subvector(uber_RB_size, temp_vector);
          get_rb_evaluation().RB_Fq_vector[q_f](i) = temp_vector.dot(der_rb_eval.derived_basis_functions[i]);
        }
    }

  DenseMatrix<Number> temp_matrix;
  for(unsigned int i=(derived_RB_size-delta_N); i<derived_RB_size; i++)
    {
      for(unsigned int n=0; n<get_rb_theta_expansion().get_n_outputs(); n++)
        for(unsigned int q_l=0; q_l<get_rb_theta_expansion().get_n_output_terms(n); q_l++)
          {
            uber_system.get_rb_evaluation().RB_output_vectors[n][q_l].get_principal_subvector(uber_RB_size, temp_vector);
            get_rb_evaluation().RB_output_vectors[n][q_l](i) = temp_vector.dot(der_rb_eval.derived_basis_functions[i]);
          }

      for(unsigned int j=0; j<derived_RB_size; j++)
        {
          for(unsigned int q_a=0; q_a<Q_a; q_a++)
            {
              // Compute reduced Aq matrix
              uber_system.get_rb_evaluation().RB_Aq_vector[q_a].get_principal_submatrix(uber_RB_size, temp_matrix);
              temp_matrix.vector_mult(temp_vector, der_rb_eval.derived_basis_functions[j]);
              get_rb_evaluation().RB_Aq_vector[q_a](i,j) = der_rb_eval.derived_basis_functions[i].dot(temp_vector);

              if(i!=j)
                {
                  temp_vector.zero();
                  temp_matrix.vector_mult(temp_vector, der_rb_eval.derived_basis_functions[i]);
                  get_rb_evaluation().RB_Aq_vector[q_a](j,i) = (der_rb_eval.derived_basis_functions[j]).dot(temp_vector);
                }
            }
        }
    }

  STOP_LOG("update_RB_system_matrices()", "DerivedRBConstruction");
}


template <>
void DerivedRBConstruction<RBConstruction>::generate_residual_terms_wrt_truth()
{
  START_LOG("generate_residual_terms_wrt_truth()", "DerivedRBConstruction");

  SteadyDerivedRBEvaluation& drb_eval = cast_ref< SteadyDerivedRBEvaluation& >(get_rb_evaluation());

  if(drb_eval.residual_type_flag != SteadyDerivedRBEvaluation::RESIDUAL_WRT_TRUTH)
    {
      // Set flag to compute residual wrt truth space
      drb_eval.residual_type_flag = SteadyDerivedRBEvaluation::RESIDUAL_WRT_TRUTH;

      recompute_all_residual_terms(/*compute_inner_products = */ true);
    }
  STOP_LOG("generate_residual_terms_wrt_truth()", "DerivedRBConstruction");
}

template <>
void DerivedRBConstruction<RBConstruction>::compute_Fq_representor_innerprods(bool compute_inner_products)
{
  START_LOG("compute_Fq_representor_innerprods()", "DerivedRBConstruction");

  // We don't short-circuit here even if Fq_representor_innerprods_computed = true because
  // the residual mode may have changed (this function is very cheap so not much
  // incentive to make sure we do not call it extra times)

  EquationSystems& es = this->get_equation_systems();
  RBConstruction& uber_system = es.get_system<RBConstruction>(uber_system_name);

  SteadyDerivedRBEvaluation& drb_eval = cast_ref< SteadyDerivedRBEvaluation& >(get_rb_evaluation());

  const unsigned int Q_f = get_rb_theta_expansion().get_n_F_terms();

  switch(drb_eval.residual_type_flag)
    {
    case(SteadyDerivedRBEvaluation::RESIDUAL_WRT_UBER):
      {
        unsigned int uber_RB_size = uber_system.get_rb_evaluation().get_n_basis_functions();
        DenseVector<Number> temp_vector1, temp_vector2;

        // Assume inner product matrix is the identity, hence don't need to
        // do any solves
        if (compute_inner_products)
          {
            unsigned int q=0;
            for(unsigned int q_f1=0; q_f1<Q_f; q_f1++)
              {
                for(unsigned int q_f2=q_f1; q_f2<Q_f; q_f2++)
                  {
                    uber_system.get_rb_evaluation().RB_Fq_vector[q_f2].get_principal_subvector(uber_RB_size, temp_vector1);
                    uber_system.get_rb_evaluation().RB_Fq_vector[q_f1].get_principal_subvector(uber_RB_size, temp_vector2);
                    Fq_representor_innerprods[q] = temp_vector1.dot( temp_vector2 );
                    q++;
                  }
              }
          } // end if (compute_inner_products)

        break;
      }

    case(SteadyDerivedRBEvaluation::RESIDUAL_WRT_TRUTH):
      {
        // Copy the output terms over from uber_system
        for(unsigned int n=0; n<get_rb_theta_expansion().get_n_outputs(); n++)
          {
            output_dual_innerprods[n] = uber_system.output_dual_innerprods[n];
          }

        // Copy the Fq terms over from uber_system
        Fq_representor_innerprods = uber_system.Fq_representor_innerprods;

        break;
      }

    default:
      {
        libMesh::out << "Invalid RESIDUAL_TYPE in compute_Fq_representor_innerprods" << std::endl;
        break;
      }
    }

  Fq_representor_innerprods_computed = true;

  // Copy the Fq_representor_innerprods and output_dual_innerprods to the rb_eval,
  // where they are actually needed
  // (we store them in DerivedRBConstruction as well in order to cache
  // the data and possibly save work)
  get_rb_evaluation().Fq_representor_innerprods = Fq_representor_innerprods;
  get_rb_evaluation().output_dual_innerprods = output_dual_innerprods;

  STOP_LOG("compute_Fq_representor_innerprods()", "DerivedRBConstruction");
}

template <>
void DerivedRBConstruction<RBConstruction>::update_residual_terms(bool compute_inner_products)
{
  START_LOG("update_residual_terms()", "DerivedRBConstruction");

  DerivedRBEvaluation<RBEvaluation>& der_rb_eval =
    cast_ref<DerivedRBEvaluation<RBEvaluation>&>(get_rb_evaluation());

  EquationSystems& es = this->get_equation_systems();
  RBConstruction& uber_system = es.get_system<RBConstruction>(uber_system_name);

  const unsigned int Q_a = get_rb_theta_expansion().get_n_A_terms();
  const unsigned int Q_f = get_rb_theta_expansion().get_n_F_terms();

  switch(der_rb_eval.residual_type_flag)
    {
    case(SteadyDerivedRBEvaluation::RESIDUAL_WRT_UBER):
      {
        unsigned int derived_RB_size = get_rb_evaluation().get_n_basis_functions();
        unsigned int uber_RB_size = uber_system.get_rb_evaluation().get_n_basis_functions();
        DenseVector<Number> temp_vector1, temp_vector2;

        // Now compute and store the inner products (if requested)
        if (compute_inner_products)
          {

            DenseMatrix<Number> temp_matrix;
            for(unsigned int q_f=0; q_f<Q_f; q_f++)
              {
                for(unsigned int q_a=0; q_a<Q_a; q_a++)
                  {
                    for(unsigned int i=(derived_RB_size-delta_N); i<derived_RB_size; i++)
                      {
                        uber_system.get_rb_evaluation().RB_Aq_vector[q_a].get_principal_submatrix(uber_RB_size, temp_matrix);
                        temp_matrix.vector_mult(temp_vector1, der_rb_eval.derived_basis_functions[i]);
                        uber_system.get_rb_evaluation().RB_Fq_vector[q_f].get_principal_subvector(uber_RB_size, temp_vector2);
                        get_rb_evaluation().Fq_Aq_representor_innerprods[q_f][q_a][i] = -temp_vector1.dot(temp_vector2);
                      }
                  }
              }

            unsigned int q=0;
            for(unsigned int q_a1=0; q_a1<Q_a; q_a1++)
              {
                for(unsigned int q_a2=q_a1; q_a2<Q_a; q_a2++)
                  {
                    for(unsigned int i=(derived_RB_size-delta_N); i<derived_RB_size; i++)
                      {
                        for(unsigned int j=0; j<derived_RB_size; j++)
                          {
                            uber_system.get_rb_evaluation().RB_Aq_vector[q_a1].get_principal_submatrix(uber_RB_size, temp_matrix);
                            temp_matrix.vector_mult(temp_vector1, der_rb_eval.derived_basis_functions[i]);
                            uber_system.get_rb_evaluation().RB_Aq_vector[q_a2].get_principal_submatrix(uber_RB_size, temp_matrix);
                            temp_matrix.vector_mult(temp_vector2, der_rb_eval.derived_basis_functions[j]);
                            get_rb_evaluation().Aq_Aq_representor_innerprods[q][i][j] = temp_vector1.dot(temp_vector2);

                            if(i != j)
                              {
                                uber_system.get_rb_evaluation().RB_Aq_vector[q_a1].get_principal_submatrix(uber_RB_size, temp_matrix);
                                temp_matrix.vector_mult(temp_vector1, der_rb_eval.derived_basis_functions[j]);
                                uber_system.get_rb_evaluation().RB_Aq_vector[q_a2].get_principal_submatrix(uber_RB_size, temp_matrix);
                                temp_matrix.vector_mult(temp_vector2, der_rb_eval.derived_basis_functions[i]);
                                get_rb_evaluation().Aq_Aq_representor_innerprods[q][j][i] = temp_vector1.dot(temp_vector2);
                              }
                          }
                      }
                    q++;
                  }
              }
          } // end if (compute_inner_products)

        break;
      }

    case(SteadyDerivedRBEvaluation::RESIDUAL_WRT_TRUTH):
      {
        unsigned int RB_size = get_rb_evaluation().get_n_basis_functions();

        for(unsigned int q_f=0; q_f<Q_f; q_f++)
          {
            for(unsigned int q_a=0; q_a<Q_a; q_a++)
              {
                for(unsigned int i=(RB_size-delta_N); i<RB_size; i++)
                  {
                    get_rb_evaluation().Fq_Aq_representor_innerprods[q_f][q_a][i] = 0.;
                    for(unsigned int j=0; j<uber_system.get_rb_evaluation().get_n_basis_functions(); j++) // Evaluate the dot product
                      {
                        get_rb_evaluation().Fq_Aq_representor_innerprods[q_f][q_a][i] +=
                          uber_system.get_rb_evaluation().Fq_Aq_representor_innerprods[q_f][q_a][j] *
                          der_rb_eval.derived_basis_functions[i](j);
                      }
                  }
              }
          }

        unsigned int q=0;
        for(unsigned int q_a1=0; q_a1<Q_a; q_a1++)
          {
            for(unsigned int q_a2=q_a1; q_a2<Q_a; q_a2++)
              {
                for(unsigned int i=(RB_size-delta_N); i<RB_size; i++)
                  {
                    for(unsigned int j=0; j<RB_size; j++)
                      {

                        get_rb_evaluation().Aq_Aq_representor_innerprods[q][i][j] = 0.;
                        if(i != j)
                          get_rb_evaluation().Aq_Aq_representor_innerprods[q][j][i] = 0.;

                        for(unsigned int k=0; k<uber_system.get_rb_evaluation().get_n_basis_functions(); k++)
                          for(unsigned int k_prime=0; k_prime<uber_system.get_rb_evaluation().get_n_basis_functions(); k_prime++)
                            {
                              get_rb_evaluation().Aq_Aq_representor_innerprods[q][i][j] +=
                                der_rb_eval.derived_basis_functions[i](k)*der_rb_eval.derived_basis_functions[j](k_prime)*
                                uber_system.get_rb_evaluation().Aq_Aq_representor_innerprods[q][k][k_prime];

                              if(i != j)
                                {
                                  get_rb_evaluation().Aq_Aq_representor_innerprods[q][j][i] +=
                                    der_rb_eval.derived_basis_functions[j](k)*der_rb_eval.derived_basis_functions[i](k_prime)*
                                    uber_system.get_rb_evaluation().Aq_Aq_representor_innerprods[q][k][k_prime];
                                }
                            }
                      }
                  }
                q++;
              }
          }

        break;
      }

    default:
      {
        libMesh::out << "Invalid RESIDUAL_TYPE in update_residual_terms" << std::endl;
        break;
      }
    }

  STOP_LOG("update_residual_terms()", "DerivedRBConstruction");
}

template<>
void DerivedRBConstruction<RBConstruction>::load_rb_solution()
{
  START_LOG("load_rb_solution()", "DerivedRBConstruction");

  solution->zero();

  if(get_rb_evaluation().RB_solution.size() > get_rb_evaluation().get_n_basis_functions())
    libmesh_error_msg("ERROR: rb_eval contains " << get_rb_evaluation().get_n_basis_functions() << " basis functions." \
                      << " RB_solution vector constains " << get_rb_evaluation().RB_solution.size() << " entries." \
                      << " RB_solution in RBConstruction::load_rb_solution is too long!");

  DerivedRBEvaluation<RBEvaluation>& der_rb_eval =
    cast_ref<DerivedRBEvaluation<RBEvaluation>&>(get_rb_evaluation());

  EquationSystems& es = this->get_equation_systems();
  RBConstruction& uber_system = es.get_system<RBConstruction>(uber_system_name);

  for(unsigned int i=0; i<get_rb_evaluation().RB_solution.size(); i++)
    for(unsigned int j=0; j<uber_system.get_rb_evaluation().get_n_basis_functions(); j++)
      {
        solution->add(get_rb_evaluation().RB_solution(i)*der_rb_eval.derived_basis_functions[i](j),
                      uber_system.get_rb_evaluation().get_basis_function(j));
      }

  update();

  STOP_LOG("load_rb_solution()", "DerivedRBConstruction");
}

}
