#include <cassert>

#include "atidlas/array.h"
#include "atidlas/backend/templates/vaxpy.h"
#include "atidlas/backend/templates/reduction.h"
#include "atidlas/backend/templates/maxpy.h"
#include "atidlas/backend/templates/mreduction.h"
#include "atidlas/backend/templates/mproduct.h"
#include "atidlas/backend/templates/base.h"
#include "atidlas/backend/parse.h"
#include "atidlas/exception/operation_not_supported.h"
#include "atidlas/exception/unknown_datatype.h"
#include "atidlas/tools/to_string.hpp"
#include "atidlas/tools/make_map.hpp"
#include "atidlas/symbolic/io.h"

namespace atidlas
{

base::parameters_type::parameters_type(unsigned int _simd_width, int_t _local_size_1, int_t _local_size_2, int_t _num_kernels) : simd_width(_simd_width), local_size_0(_local_size_1), local_size_1(_local_size_2), num_kernels(_num_kernels)
{ }

numeric_type base::map_functor::get_numeric_type(atidlas::symbolic_expression const * symbolic_expression, int_t root_idx) const
{
  symbolic_expression_node const * root_node = &symbolic_expression->tree()[root_idx];
  while (root_node->lhs.dtype==INVALID_NUMERIC_TYPE)
    root_node = &symbolic_expression->tree()[root_node->lhs.node_index];
  return root_node->lhs.dtype;
}

/** @brief Binary leaf */
template<class T>
tools::shared_ptr<mapped_object> base::map_functor::binary_leaf(atidlas::symbolic_expression const * symbolic_expression, int_t root_idx, mapping_type const * mapping) const
{
  return tools::shared_ptr<mapped_object>(new T(numeric_type_to_string(symbolic_expression->dtype()), binder_.get(NULL), mapped_object::node_info(mapping, symbolic_expression, root_idx)));
}

/** @brief Scalar mapping */
tools::shared_ptr<mapped_object> base::map_functor::create(numeric_type dtype, values_holder) const
{
  std::string strdtype = numeric_type_to_string(dtype);
  return tools::shared_ptr<mapped_object>(new mapped_host_scalar(strdtype, binder_.get(NULL)));
}

/** @brief Vector mapping */
tools::shared_ptr<mapped_object> base::map_functor::create(array_infos const & a) const
{
  std::string dtype = numeric_type_to_string(a.dtype);
  unsigned int id = binder_.get(a.data);
  //Scalar
  if(a.shape1==1 && a.shape2==1)
    return tools::shared_ptr<mapped_object>(new mapped_array(dtype, id, 's'));
  //Column vector
  else if(a.shape1>1 && a.shape2==1)
    return tools::shared_ptr<mapped_object>(new mapped_array(dtype, id, 'c'));
  //Row vector
  else if(a.shape1==1 && a.shape2>1)
    return tools::shared_ptr<mapped_object>(new mapped_array(dtype, id, 'r'));
  //Matrix
  else
    return tools::shared_ptr<mapped_object>(new mapped_array(dtype, id, 'm'));
}

tools::shared_ptr<mapped_object> base::map_functor::create(repeat_infos const &) const
{
  //TODO: Make it less specific!
  return tools::shared_ptr<mapped_object>(new mapped_tuple("int",binder_.get(NULL),4));
}

tools::shared_ptr<mapped_object> base::map_functor::create(lhs_rhs_element const & lhs_rhs) const
{
  switch(lhs_rhs.type_family)
  {
    case INFOS_TYPE_FAMILY: return create(lhs_rhs.tuple);
    case VALUE_TYPE_FAMILY: return create(lhs_rhs.dtype, lhs_rhs.vscalar);
    case ARRAY_TYPE_FAMILY: return create(lhs_rhs.array);
    default: throw "";
  }
}


base::map_functor::map_functor(symbolic_binder & binder, mapping_type & mapping) : binder_(binder), mapping_(mapping){ }

/** @brief Traversal functor */
void base::map_functor::operator()(atidlas::symbolic_expression const & symbolic_expression, int_t root_idx, leaf_t leaf_t) const {
  mapping_type::key_type key(root_idx, leaf_t);
  symbolic_expression_node const & root_node = symbolic_expression.tree()[root_idx];

  if (leaf_t == LHS_NODE_TYPE && root_node.lhs.type_family != COMPOSITE_OPERATOR_FAMILY)
    mapping_.insert(mapping_type::value_type(key, create(root_node.lhs)));
  else if (leaf_t == RHS_NODE_TYPE && root_node.rhs.type_family != COMPOSITE_OPERATOR_FAMILY)
    mapping_.insert(mapping_type::value_type(key, create(root_node.rhs)));
  else if ( leaf_t== PARENT_NODE_TYPE)
  {
    if (root_node.op.type==OPERATOR_VDIAG_TYPE)
      mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_vdiag>(&symbolic_expression, root_idx, &mapping_)));
    else if (root_node.op.type==OPERATOR_MATRIX_DIAG_TYPE)
      mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_matrix_diag>(&symbolic_expression, root_idx, &mapping_)));
    else if (root_node.op.type==OPERATOR_MATRIX_ROW_TYPE)
      mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_matrix_row>(&symbolic_expression, root_idx, &mapping_)));
    else if (root_node.op.type==OPERATOR_MATRIX_COLUMN_TYPE)
      mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_matrix_column>(&symbolic_expression, root_idx, &mapping_)));
    else if (detail::is_scalar_reduction(root_node))
      mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_scalar_reduction>(&symbolic_expression, root_idx, &mapping_)));
    else if (detail::is_vector_reduction(root_node))
      mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_mreduction>(&symbolic_expression, root_idx, &mapping_)));
    else if (root_node.op.type_family == OPERATOR_MATRIX_PRODUCT_TYPE_FAMILY)
      mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_mproduct>(&symbolic_expression, root_idx, &mapping_)));
    else if (root_node.op.type == OPERATOR_REPEAT_TYPE)
      mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_repeat>(&symbolic_expression, root_idx, &mapping_)));
    else if (root_node.op.type == OPERATOR_OUTER_PROD_TYPE)
      mapping_.insert(mapping_type::value_type(key, binary_leaf<mapped_outer>(&symbolic_expression, root_idx, &mapping_)));
  }
}

base::set_arguments_functor::set_arguments_functor(symbolic_binder & binder, unsigned int & current_arg, cl::Kernel & kernel) :
  binder_(binder), current_arg_(current_arg), kernel_(kernel){ }

void base::set_arguments_functor::set_arguments(numeric_type dtype, values_holder const & scal) const
{
  switch(dtype)
  {
    case CHAR_TYPE: kernel_.setArg(current_arg_++, scal.int8); break;
    case UCHAR_TYPE: kernel_.setArg(current_arg_++, scal.uint8); break;
    case SHORT_TYPE: kernel_.setArg(current_arg_++, scal.int16); break;
    case USHORT_TYPE: kernel_.setArg(current_arg_++, scal.uint16); break;
    case INT_TYPE: kernel_.setArg(current_arg_++, scal.int32); break;
    case UINT_TYPE: kernel_.setArg(current_arg_++, scal.uint32); break;
    case LONG_TYPE: kernel_.setArg(current_arg_++, scal.int64); break;
    case ULONG_TYPE: kernel_.setArg(current_arg_++, scal.uint64); break;
    case FLOAT_TYPE: kernel_.setArg(current_arg_++, scal.float32); break;
    case DOUBLE_TYPE: kernel_.setArg(current_arg_++, scal.float64); break;
    default: throw unknown_datatype(dtype);
  }
}

/** @brief Vector mapping */
void base::set_arguments_functor::set_arguments(array_infos const & x) const
{
  bool is_bound = binder_.bind(x.data);
  if (is_bound)
  {
    kernel_.setArg(current_arg_++, x.data);
    //scalar
    if(x.shape1==1 && x.shape2==1)
    {
      kernel_.setArg(current_arg_++, cl_uint(x.start1));
    }
    //array
    else if(x.shape1==1 || x.shape2==1)
    {
      kernel_.setArg(current_arg_++, cl_uint(std::max(x.start1, x.start2)));
      kernel_.setArg(current_arg_++, cl_uint(std::max(x.stride1, x.stride2)));
    }
    else
    {
      kernel_.setArg(current_arg_++, cl_uint(x.ld));
      kernel_.setArg(current_arg_++, cl_uint(x.start1));
      kernel_.setArg(current_arg_++, cl_uint(x.start2));
      kernel_.setArg(current_arg_++, cl_uint(x.stride1));
      kernel_.setArg(current_arg_++, cl_uint(x.stride2));
    }
  }
}

void base::set_arguments_functor::set_arguments(repeat_infos const & i) const
{
  kernel_.setArg(current_arg_++, cl_uint(i.sub1));
  kernel_.setArg(current_arg_++, cl_uint(i.sub2));
  kernel_.setArg(current_arg_++, cl_uint(i.rep1));
  kernel_.setArg(current_arg_++, cl_uint(i.rep2));
}

void base::set_arguments_functor::set_arguments(lhs_rhs_element const & lhs_rhs) const
{
  switch(lhs_rhs.type_family)
  {
    case VALUE_TYPE_FAMILY: return set_arguments(lhs_rhs.dtype, lhs_rhs.vscalar);
    case ARRAY_TYPE_FAMILY: return set_arguments(lhs_rhs.array);
    case INFOS_TYPE_FAMILY: return set_arguments(lhs_rhs.tuple);
    default: throw invalid_exception("Unrecognized type family");
  }
}

/** @brief Traversal functor: */
void base::set_arguments_functor::operator()(atidlas::symbolic_expression const & symbolic_expression, int_t root_idx, leaf_t leaf_t) const
{
  symbolic_expression_node const & root_node = symbolic_expression.tree()[root_idx];
  if (leaf_t==LHS_NODE_TYPE && root_node.lhs.type_family != COMPOSITE_OPERATOR_FAMILY)
    set_arguments(root_node.lhs);
  else if (leaf_t==RHS_NODE_TYPE && root_node.rhs.type_family != COMPOSITE_OPERATOR_FAMILY)
    set_arguments(root_node.rhs);
}

void base::compute_reduction(kernel_generation_stream & os, std::string acc, std::string cur, op_element const & op)
{
  if (detail::is_elementwise_function(op))
    os << acc << "=" << evaluate(op.type) << "(" << acc << "," << cur << ");" << std::endl;
  else
    os << acc << "= (" << acc << ")" << evaluate(op.type)  << "(" << cur << ");" << std::endl;
}

void base::compute_index_reduction(kernel_generation_stream & os, std::string acc, std::string cur, std::string const & acc_value, std::string const & cur_value, op_element const & op)
{
  //        os << acc << " = " << cur_value << ">" << acc_value  << "?" << cur << ":" << acc << ";" << std::endl;
  os << acc << "= select(" << acc << "," << cur << "," << cur_value << ">" << acc_value << ");" << std::endl;
  os << acc_value << "=";
  if (op.type==OPERATOR_ELEMENT_ARGFMAX_TYPE) os << "fmax";
  if (op.type==OPERATOR_ELEMENT_ARGMAX_TYPE) os << "max";
  if (op.type==OPERATOR_ELEMENT_ARGFMIN_TYPE) os << "fmin";
  if (op.type==OPERATOR_ELEMENT_ARGMIN_TYPE) os << "min";
  os << "(" << acc_value << "," << cur_value << ");"<< std::endl;
}

void base::process_all(std::string const & type_key, std::string const & str,
                        kernel_generation_stream & stream, std::vector<mapping_type> const & mappings)
{
  for (std::vector<mapping_type>::const_iterator mit = mappings.begin(); mit != mappings.end(); ++mit)
    for (mapping_type::const_iterator mmit = mit->begin(); mmit != mit->end(); ++mmit)
      if (mmit->second->type_key()==type_key)
        stream << mmit->second->process(str) << std::endl;
}


void base::base::process_all_at(std::string const & type_key, std::string const & str,
                           kernel_generation_stream & stream, std::vector<mapping_type> const & mappings,
                           size_t root_idx, leaf_t leaf)
{
  for (std::vector<mapping_type>::const_iterator mit = mappings.begin(); mit != mappings.end(); ++mit)
  {
    mapped_object * obj = mit->at(mapping_key(root_idx, leaf)).get();
    if (obj->type_key()==type_key)
      stream << obj->process(str) << std::endl;
  }
}

std::string base::neutral_element(op_element const & op)
{
  switch (op.type)
  {
  case OPERATOR_ADD_TYPE : return "0";
  case OPERATOR_MULT_TYPE : return "1";
  case OPERATOR_DIV_TYPE : return "1";
  case OPERATOR_ELEMENT_FMAX_TYPE : return "-INFINITY";
  case OPERATOR_ELEMENT_ARGFMAX_TYPE : return "-INFINITY";
  case OPERATOR_ELEMENT_MAX_TYPE : return "-INFINITY";
  case OPERATOR_ELEMENT_ARGMAX_TYPE : return "-INFINITY";
  case OPERATOR_ELEMENT_FMIN_TYPE : return "INFINITY";
  case OPERATOR_ELEMENT_ARGFMIN_TYPE : return "INFINITY";
  case OPERATOR_ELEMENT_MIN_TYPE : return "INFINITY";
  case OPERATOR_ELEMENT_ARGMIN_TYPE : return "INFINITY";

  default: throw operation_not_supported_exception("Unsupported reduction operator : no neutral element known");
  }
}

std::string base::generate_arguments(std::vector<mapping_type> const & mappings, std::map<std::string, std::string> const & accessors, symbolic_expressions_container const & symbolic_expressions)
{
  kernel_generation_stream stream;
  process(stream, PARENT_NODE_TYPE, accessors, symbolic_expressions, mappings);
  std::string res = stream.str();
  res.erase(res.rfind(','));
  return res;
}

std::string base::generate_arguments(std::string const & data_type, std::vector<mapping_type> const & mappings, symbolic_expressions_container const & symbolic_expressions)
{
  return generate_arguments(mappings, tools::make_map<std::map<std::string, std::string> >("array0", "__global #scalartype* #pointer, uint #start,")
                                                                    ("host_scalar", "#scalartype #name,")
                                                                    ("array1", "__global " + data_type + "* #pointer, uint #start, uint #stride,")
                                                                    ("array2", "__global " + data_type + "* #pointer, uint #ld, uint #start1, uint #start2, uint #stride1, uint #stride2,")
                                                                    ("tuple4", "#scalartype #name0, #scalartype #name1, #scalartype #name2, #scalartype #name3,"), symbolic_expressions);
}



void base::set_arguments(symbolic_expressions_container const & symbolic_expressions, cl::Kernel & kernel, unsigned int & current_arg)
{
  tools::shared_ptr<symbolic_binder> binder = make_binder();
  for (symbolic_expressions_container::data_type::const_iterator itt = symbolic_expressions.data().begin(); itt != symbolic_expressions.data().end(); ++itt)
    traverse(**itt, (*itt)->root(), set_arguments_functor(*binder, current_arg, kernel), true);
}

void base::fill_kernel_name(char * ptr, unsigned int label, const char * suffix)
{
  *ptr++='k';
  if (label==0)
    *ptr++='0';
  else
    while (label>0)
    {
      *ptr++= (char)('0' + (label % 10));
      label /= 10;
    }
  for(std::size_t i = 0 ; i < strlen(suffix);++i)
    *ptr++=suffix[i];
  *ptr++='\0';
}

base::invalid_exception::invalid_exception() : message_() {}

base::invalid_exception::invalid_exception(std::string message) :
  message_("ViennaCL: Internal error: The generator cannot apply the given template to the given symbolic_expression: " + message + "\n"
           "If you are using a builtin template, please report on viennacl-support@lists.sourceforge.net! We will provide a fix as soon as possible\n"
           "If you are using your own template, please try using other parameters") {}

const char* base::invalid_exception::what() const throw() { return message_.c_str(); }

base::invalid_exception::~invalid_exception() throw() {}

void base::fetching_loop_info(fetching_policy_type policy, std::string const & bound, kernel_generation_stream & stream, std::string & init, std::string & upper_bound, std::string & inc, std::string const & domain_id, std::string const & domain_size)
{
  if (policy==FETCH_FROM_GLOBAL_STRIDED)
  {
    init = domain_id;
    upper_bound = bound;
    inc = domain_size;
  }
  else if (policy==FETCH_FROM_GLOBAL_CONTIGUOUS)
  {
    std::string chunk_size = "chunk_size";
    std::string chunk_start = "chunk_start";
    std::string chunk_end = "chunk_end";

    stream << "unsigned int " << chunk_size << " = (" << bound << "+" << domain_size << "-1)/" << domain_size << ";" << std::endl;
    stream << "unsigned int " << chunk_start << " =" << domain_id << "*" << chunk_size << ";" << std::endl;
    stream << "unsigned int " << chunk_end << " = min(" << chunk_start << "+" << chunk_size << ", " << bound << ");" << std::endl;
    init = chunk_start;
    upper_bound = chunk_end;
    inc = "1";
  }
}

bool base::is_node_trans(symbolic_expression::container_type const & array, size_t root_idx, leaf_t leaf_type)
{
  bool res = false;
  lhs_rhs_element symbolic_expression_node::*ptr;
  if (leaf_type==LHS_NODE_TYPE)
    ptr = &symbolic_expression_node::lhs;
  else
    ptr = &symbolic_expression_node::rhs;
  symbolic_expression_node const * node = &array[root_idx];
  while ((node->*ptr).type_family==COMPOSITE_OPERATOR_FAMILY)
  {
    if (array[(node->*ptr).node_index].op.type==OPERATOR_TRANS_TYPE)
      res = !res;
    node = &array[(node->*ptr).node_index];
  }
  return res;
}

std::string base::append_simd_suffix(std::string const & str, unsigned int i)
{
  assert(i < 16);
  char suffixes[] = {'0','1','2','3','4','5','6','7','8','9',
                           'a','b','c','d','e','f'};
  return str + tools::to_string(suffixes[i]);
}

bool base::is_strided(symbolic_expression_node const & node)
{
  return node.op.type==OPERATOR_VDIAG_TYPE
      || node.op.type==OPERATOR_MATRIX_DIAG_TYPE
      || node.op.type==OPERATOR_MATRIX_ROW_TYPE
      || node.op.type==OPERATOR_MATRIX_COLUMN_TYPE
      || node.op.type==OPERATOR_OUTER_PROD_TYPE;
}

bool base::requires_fallback(symbolic_expressions_container const & symbolic_expressions)
{
  for (symbolic_expressions_container::data_type::const_iterator it = symbolic_expressions.data().begin(); it != symbolic_expressions.data().end(); ++it)
    for(symbolic_expression::container_type::const_iterator itt = (*it)->tree().begin(); itt != (*it)->tree().end() ; ++itt)
      if(   (itt->lhs.subtype==DENSE_ARRAY_TYPE && (std::max(itt->lhs.array.stride1, itt->lhs.array.stride2)>1 || std::max(itt->lhs.array.start1,itt->lhs.array.start2)>0))
         || (itt->rhs.subtype==DENSE_ARRAY_TYPE && (std::max(itt->rhs.array.stride1, itt->rhs.array.stride2)>1 || std::max(itt->rhs.array.start1,itt->rhs.array.start2)>0)))
        return true;
  return false;
}

int_t base::vector_size(symbolic_expression_node const & node)
{
  using namespace tools;
  if (node.op.type==OPERATOR_MATRIX_DIAG_TYPE)
    return std::min<int_t>(node.lhs.array.shape1, node.lhs.array.shape2);
  else if (node.op.type==OPERATOR_MATRIX_ROW_TYPE)
    return node.lhs.array.shape2;
  else if (node.op.type==OPERATOR_MATRIX_COLUMN_TYPE)
    return node.lhs.array.shape1;
  else
    return std::max(node.lhs.array.shape1, node.lhs.array.shape2);

}

std::pair<int_t, int_t> base::matrix_size(symbolic_expression_node const & node)
{
  if (node.op.type==OPERATOR_VDIAG_TYPE)
  {
    int_t size = node.lhs.array.shape1;
    return std::make_pair(size,size);
  }
  else if(node.op.type==OPERATOR_REPEAT_TYPE)
    return std::make_pair(node.lhs.array.shape1*node.rhs.tuple.rep1, node.lhs.array.shape2*node.rhs.tuple.rep2);
  else
    return std::make_pair(node.lhs.array.shape1,node.lhs.array.shape2);
}

void base::element_wise_loop_1D(kernel_generation_stream & stream, loop_body_base const & loop_body,
                                 fetching_policy_type fetch, unsigned int simd_width, std::string const & i, std::string const & bound, std::string const & domain_id, std::string const & domain_size)
{
  std::string strwidth = tools::to_string(simd_width);
  std::string boundround = bound + "/" + strwidth;

  std::string init, upper_bound, inc;
  fetching_loop_info(fetch, boundround, stream, init, upper_bound, inc, domain_id, domain_size);
  stream << "for(unsigned int " << i << " = " << init << "; " << i << " < " << upper_bound << "; " << i << " += " << inc << ")" << std::endl;
  stream << "{" << std::endl;
  stream.inc_tab();
  loop_body(stream, simd_width);
  stream.dec_tab();
  stream << "}" << std::endl;

  if (simd_width>1)
  {
    stream << "for(unsigned int " << i << " = " << boundround << "*" << strwidth << " + " << domain_id << "; " << i << " < " << bound << "; " << i << " += " + domain_size + ")" << std::endl;
    stream << "{" << std::endl;
    stream.inc_tab();
    loop_body(stream, 1);
    stream.dec_tab();
    stream << "}" << std::endl;
  }
}

bool base::is_reduction(symbolic_expression_node const & node)
{
  return node.op.type_family==OPERATOR_VECTOR_REDUCTION_TYPE_FAMILY
      || node.op.type_family==OPERATOR_COLUMNS_REDUCTION_TYPE_FAMILY
      || node.op.type_family==OPERATOR_ROWS_REDUCTION_TYPE_FAMILY;
}

bool base::is_index_reduction(op_element const & op)
{
  return op.type==OPERATOR_ELEMENT_ARGFMAX_TYPE
      || op.type==OPERATOR_ELEMENT_ARGMAX_TYPE
      || op.type==OPERATOR_ELEMENT_ARGFMIN_TYPE
      || op.type==OPERATOR_ELEMENT_ARGMIN_TYPE;
}

std::string base::vstore(unsigned int simd_width, std::string const & value, std::string const & offset, std::string const & ptr)
{
  if (simd_width==1)
    return "(" + ptr + ")[" + offset + "] = " + value;
  else
    return append_width("vstore", simd_width) + "(" + value + ", " + offset + ", " + ptr + ")";
}

std::string base::vload(unsigned int simd_width, std::string const & offset, std::string const & ptr)
{
  if (simd_width==1)
    return "(" + ptr + ")[" + offset + "]";
  else
    return append_width("vload", simd_width) + "(" + offset + ", " + ptr + ")";
}

std::string base::append_width(std::string const & str, unsigned int width)
{
  if (width==1)
    return str;
  return str + tools::to_string(width);
}

unsigned int base::align(unsigned int to_round, unsigned int base)
{
  if (to_round % base == 0)
    return to_round;
  return (to_round + base - 1)/base * base;
}

tools::shared_ptr<symbolic_binder> base::make_binder()
{
  if (binding_policy_==BIND_TO_HANDLE)
    return tools::shared_ptr<symbolic_binder>(new bind_to_handle());
  else
    return tools::shared_ptr<symbolic_binder>(new bind_all_unique());
}


base::base(binding_policy_t binding_policy) : binding_policy_(binding_policy)
{}

unsigned int base::lmem_usage(symbolic_expressions_container const &) const
{ return 0; }

unsigned int base::registers_usage(symbolic_expressions_container const &) const
{ return 0; }

base::~base()
{ }

std::vector<std::string> base::generate(unsigned int label, symbolic_expressions_container const & symbolic_expressions, cl::Device const & device)
{
  symbolic_expressions_container::data_type::const_iterator sit;
  std::vector<mapping_type>::iterator mit;

  if(int err = check_invalid(symbolic_expressions, device))
    throw operation_not_supported_exception("The supplied parameters for this template are invalid : err " + tools::to_string(err));

  //Create mapping
  std::vector<mapping_type> mappings(symbolic_expressions.data().size());
  tools::shared_ptr<symbolic_binder> binder = make_binder();
  for (mit = mappings.begin(), sit = symbolic_expressions.data().begin(); sit != symbolic_expressions.data().end(); ++sit, ++mit)
    traverse(**sit, (*sit)->root(), map_functor(*binder,*mit), true);

  return generate_impl(label, symbolic_expressions, mappings);
}

template<class TType, class PType>
int base_impl<TType, PType>::check_invalid_impl(cl::Device const &, symbolic_expressions_container const &) const
{ return TEMPLATE_VALID; }

template<class TType, class PType>
base_impl<TType, PType>::base_impl(parameters_type const & parameters, binding_policy_t binding_policy) : base(binding_policy), p_(parameters)
{ }

template<class TType, class PType>
int_t base_impl<TType, PType>::local_size_0() const
{ return p_.local_size_0; }

template<class TType, class PType>
int_t base_impl<TType, PType>::local_size_1() const
{ return p_.local_size_1; }

template<class TType, class PType>
tools::shared_ptr<base> base_impl<TType, PType>::clone() const
{ return tools::shared_ptr<base>(new TType(*dynamic_cast<TType const *>(this))); }

template<class TType, class PType>
int base_impl<TType, PType>::check_invalid(symbolic_expressions_container const & symbolic_expressions, cl::Device const & device) const
{
  //Query device informations
  size_t lmem_available = device.getInfo<CL_DEVICE_LOCAL_MEM_SIZE>();
  size_t lmem_used = lmem_usage(symbolic_expressions);
  if (lmem_used>lmem_available)
    return TEMPLATE_LOCAL_MEMORY_OVERFLOW;

  //Invalid work group size
  size_t max_workgroup_size = device.getInfo<CL_DEVICE_MAX_WORK_GROUP_SIZE>();
  std::vector<size_t> max_work_item_sizes = device.getInfo<CL_DEVICE_MAX_WORK_ITEM_SIZES>();
  if (p_.local_size_0*p_.local_size_1 > max_workgroup_size)
    return TEMPLATE_WORK_GROUP_SIZE_OVERFLOW;
  if (p_.local_size_0 > max_work_item_sizes[0])
    return TEMPLATE_LOCAL_SIZE_0_OVERFLOW;

  if (p_.local_size_1 > max_work_item_sizes[1])
    return TEMPLATE_LOCAL_SIZE_1_OVERFLOW;

  //Advice from the Intel guide
  unsigned int warp_size = 8;
  if (device.getInfo<CL_DEVICE_TYPE>()==CL_DEVICE_TYPE_GPU)
  {
    //Advice from the nvidia guide
    warp_size = 32;
    //Advice from the AMD guide
    if (device.getInfo<CL_DEVICE_VENDOR_ID>()==4098)
      warp_size = 64;
  }
  if (((p_.local_size_0*p_.local_size_1)%warp_size)>0)
    return TEMPLATE_LOCAL_SIZE_NOT_WARP_MULTIPLE;

  //Invalid SIMD Width
  if (p_.simd_width!=1 && p_.simd_width!=2 &&
      p_.simd_width!=4 && p_.simd_width!=8 &&
      p_.simd_width!=16)
    return TEMPLATE_INVALID_SIMD_WIDTH;

  return check_invalid_impl(device, symbolic_expressions);
}

template class base_impl<vaxpy, vaxpy_parameters>;
template class base_impl<reduction, reduction_parameters>;
template class base_impl<maxpy, maxpy_parameters>;
template class base_impl<mreduction, mreduction_parameters>;
template class base_impl<mproduct, mproduct_parameters>;

}
