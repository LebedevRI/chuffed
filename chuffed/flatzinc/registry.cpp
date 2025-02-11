/*
 *  Permission is hereby granted, free of charge, to any person obtaining
 *  a copy of this software and associated documentation files (the
 *  "Software"), to deal in the Software without restriction, including
 *  without limitation the rights to use, copy, modify, merge, publish,
 *  distribute, sublicense, and/or sell copies of the Software, and to
 *  permit persons to whom the Software is furnished to do so, subject to
 *  the following conditions:
 *
 *  The above copyright notice and this permission notice shall be
 *  included in all copies or substantial portions of the Software.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 *  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 *  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 *  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 *  LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 *  OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 *  WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 */

#include "chuffed/core/engine.h"
#include "chuffed/core/propagator.h"
#include "chuffed/core/sat-types.h"
#include "chuffed/core/sat.h"
#include "chuffed/flatzinc/ast.h"
#include "chuffed/flatzinc/flatzinc.h"
#include "chuffed/globals/globals.h"
#include "chuffed/globals/mddglobals.h"
#include "chuffed/ldsb/ldsb.h"
#include "chuffed/mdd/opts.h"
#include "chuffed/primitives/primitives.h"
#include "chuffed/support/misc.h"
#include "chuffed/support/vec.h"
#include "chuffed/vars/bool-view.h"
#include "chuffed/vars/int-var.h"

#include <cassert>
#include <iostream>
#include <list>
#include <ostream>
#include <string>

namespace FlatZinc {

Registry& registry() {
	static Registry r;
	return r;
}

void Registry::post(const ConExpr& ce, AST::Node* ann) {
	auto i = r.find(ce.id);
	if (i == r.end()) {
		throw FlatZinc::Error("Registry", std::string("Constraint ") + ce.id + " not found");
	}
	i->second(ce, ann);
}

void Registry::add(const std::string& id, poster p) { r[id] = p; }

namespace {

ConLevel ann2icl(AST::Node* ann) {
	if (ann != nullptr) {
		if ((ann != nullptr) && ann->hasAtom("val")) {
			return CL_VAL;
		}
		if ((ann != nullptr) && (ann->hasAtom("bounds") || ann->hasAtom("boundsR") ||
														 ann->hasAtom("boundsD") || ann->hasAtom("boundsZ"))) {
			return CL_BND;
		}
		if ((ann != nullptr) && ann->hasAtom("domain")) {
			return CL_DOM;
		}
	}
	return CL_DEF;
}

inline void arg2intargs(vec<int>& ia, AST::Node* arg) {
	AST::Array* a = arg->getArray();
	ia.growTo(a->a.size());
	for (int i = a->a.size(); (i--) != 0;) {
		ia[i] = a->a[i]->getInt();
	}
}

inline void arg2boolargs(vec<bool>& ia, AST::Node* arg) {
	AST::Array* a = arg->getArray();
	ia.growTo(a->a.size());
	for (int i = a->a.size(); (i--) != 0;) {
		ia[i] = a->a[i]->getBool();
	}
}

inline void arg2intvarargs(vec<IntVar*>& ia, AST::Node* arg) {
	AST::Array* a = arg->getArray();
	ia.growTo(a->a.size());
	for (int i = a->a.size(); (i--) != 0;) {
		if (a->a[i]->isIntVar()) {
			ia[i] = s->iv[a->a[i]->getIntVar()];
		} else {
			const int value = a->a[i]->getInt();
			ia[i] = getConstant(value);
		}
	}
}

inline void arg2BoolVarArgs(vec<BoolView>& ia, AST::Node* arg) {
	AST::Array* a = arg->getArray();
	ia.growTo(a->a.size());
	for (int i = a->a.size(); (i--) != 0;) {
		if (a->a[i]->isBoolVar()) {
			ia[i] = s->bv[a->a[i]->getBoolVar()];
		} else {
			ia[i] = a->a[i]->getBool() ? bv_true : bv_false;
		}
	}
}

inline MDDOpts getMDDOpts(AST::Node* ann) {
	MDDOpts mopts;
	if (ann != nullptr) {
		if (ann->hasCall("mdd")) {
			AST::Array* args(ann->getCall("mdd")->getArray());
			for (auto& i : args->a) {
				if (auto* at = dynamic_cast<AST::Atom*>(i)) {
					mopts.parse_arg(at->id);
				}
			}
		}
	}
	return mopts;
}

std::list<std::string> getCumulativeOptions(AST::Node* ann) {
	std::list<std::string> opt;
	if (ann != nullptr) {
		if (ann->hasCall("tt_filt")) {
			if (ann->getCall("tt_filt")->args->getBool()) {
				opt.emplace_back("tt_filt_on");
			} else {
				opt.emplace_back("tt_filt_off");
			}
		}
		if (ann->hasCall("ttef_check")) {
			if (ann->getCall("ttef_check")->args->getBool()) {
				opt.emplace_back("ttef_check_on");
			} else {
				opt.emplace_back("ttef_check_off");
			}
		}
		if (ann->hasCall("ttef_filt")) {
			if (ann->getCall("ttef_filt")->args->getBool()) {
				opt.emplace_back("ttef_filt_on");
			} else {
				opt.emplace_back("ttef_filt_off");
			}
		}
		if (ann->hasCall("name")) {
			opt.push_back("__name__" + ann->getCall("name")->args->getString());
		}
	}
	return opt;
}

BoolView getBoolVar(AST::Node* n) {
	if (n->isBoolVar()) {
		return s->bv[n->getBoolVar()];
	}
	return newBoolVar(static_cast<int>(n->getBool()), static_cast<int>(n->getBool()));
}

IntVar* getIntVar(AST::Node* n) {
	IntVar* x0;
	if (n->isIntVar()) {
		x0 = s->iv[n->getIntVar()];
	} else {
		x0 = getConstant(n->getInt());
	}
	return x0;
}
void p_int_CMP(IntRelType irt, const ConExpr& ce, AST::Node* /*ann*/) {
	if (ce[0]->isIntVar()) {
		if (ce[1]->isIntVar()) {
			int_rel(getIntVar(ce[0]), irt, getIntVar(ce[1]));
		} else {
			int_rel(getIntVar(ce[0]), irt, ce[1]->getInt());
		}
	} else {
		int_rel(getIntVar(ce[1]), -irt, ce[0]->getInt());
	}
}
void p_int_eq(const ConExpr& ce, AST::Node* ann) { p_int_CMP(IRT_EQ, ce, ann); }
void p_int_ne(const ConExpr& ce, AST::Node* ann) { p_int_CMP(IRT_NE, ce, ann); }
void p_int_ge(const ConExpr& ce, AST::Node* ann) { p_int_CMP(IRT_GE, ce, ann); }
void p_int_gt(const ConExpr& ce, AST::Node* ann) { p_int_CMP(IRT_GT, ce, ann); }
void p_int_le(const ConExpr& ce, AST::Node* ann) { p_int_CMP(IRT_LE, ce, ann); }
void p_int_lt(const ConExpr& ce, AST::Node* ann) { p_int_CMP(IRT_LT, ce, ann); }

void p_int_CMP_imp(IntRelType irt, const ConExpr& ce, AST::Node* ann) {
	if (ce[2]->isBool()) {
		if (ce[2]->getBool()) {
			p_int_CMP(irt, ce, ann);
		}
		return;
	}
	if (ce[0]->isIntVar()) {
		if (ce[1]->isIntVar()) {
			int_rel_half_reif(getIntVar(ce[0]), irt, getIntVar(ce[1]), getBoolVar(ce[2]));
		} else {
			int_rel_half_reif(getIntVar(ce[0]), irt, ce[1]->getInt(), getBoolVar(ce[2]));
		}
	} else {
		int_rel_half_reif(getIntVar(ce[1]), -irt, ce[0]->getInt(), getBoolVar(ce[2]));
	}
}
void p_int_eq_imp(const ConExpr& ce, AST::Node* ann) { p_int_CMP_imp(IRT_EQ, ce, ann); }
void p_int_ne_imp(const ConExpr& ce, AST::Node* ann) { p_int_CMP_imp(IRT_NE, ce, ann); }
void p_int_ge_imp(const ConExpr& ce, AST::Node* ann) { p_int_CMP_imp(IRT_GE, ce, ann); }
void p_int_gt_imp(const ConExpr& ce, AST::Node* ann) { p_int_CMP_imp(IRT_GT, ce, ann); }
void p_int_le_imp(const ConExpr& ce, AST::Node* ann) { p_int_CMP_imp(IRT_LE, ce, ann); }
void p_int_lt_imp(const ConExpr& ce, AST::Node* ann) { p_int_CMP_imp(IRT_LT, ce, ann); }

void p_int_CMP_reif(IntRelType irt, const ConExpr& ce, AST::Node* ann) {
	if (ce[2]->isBool()) {
		if (ce[2]->getBool()) {
			p_int_CMP(irt, ce, ann);
		} else {
			p_int_CMP(!irt, ce, ann);
		}
		return;
	}
	if (ce[0]->isIntVar()) {
		if (ce[1]->isIntVar()) {
			int_rel_reif(getIntVar(ce[0]), irt, getIntVar(ce[1]), getBoolVar(ce[2]));
		} else {
			int_rel_reif(getIntVar(ce[0]), irt, ce[1]->getInt(), getBoolVar(ce[2]));
		}
	} else {
		int_rel_reif(getIntVar(ce[1]), -irt, ce[0]->getInt(), getBoolVar(ce[2]));
	}
}

/* Comparisons */
void p_int_eq_reif(const ConExpr& ce, AST::Node* ann) { p_int_CMP_reif(IRT_EQ, ce, ann); }
void p_int_ne_reif(const ConExpr& ce, AST::Node* ann) { p_int_CMP_reif(IRT_NE, ce, ann); }
void p_int_ge_reif(const ConExpr& ce, AST::Node* ann) { p_int_CMP_reif(IRT_GE, ce, ann); }
void p_int_gt_reif(const ConExpr& ce, AST::Node* ann) { p_int_CMP_reif(IRT_GT, ce, ann); }
void p_int_le_reif(const ConExpr& ce, AST::Node* ann) { p_int_CMP_reif(IRT_LE, ce, ann); }
void p_int_lt_reif(const ConExpr& ce, AST::Node* ann) { p_int_CMP_reif(IRT_LT, ce, ann); }

/* linear (in-)equations */
void p_int_lin_CMP(IntRelType irt, const ConExpr& ce, AST::Node* /*ann*/) {
	vec<int> ia;
	arg2intargs(ia, ce[0]);
	vec<IntVar*> iv;
	arg2intvarargs(iv, ce[1]);

	/*
				if (ann2icl(ann) == CL_DOM) {
					if (irt == IRT_EQ && iv.size() == 3) {
						int_linear_dom(ia, iv, ce[2]->getInt());
						return;
					}
					fprintf(stderr, "Ignoring consistency annotation on int_linear constraint\n");
				}
	*/

	int_linear(ia, iv, irt, ce[2]->getInt(), bv_true);
}
void p_int_lin_CMP_reif(IntRelType irt, const ConExpr& ce, AST::Node* ann) {
	if (ce[3]->isBool()) {
		if (ce[3]->getBool()) {
			p_int_lin_CMP(irt, ce, ann);
		} else {
			p_int_lin_CMP(!irt, ce, ann);
		}
		return;
	}
	vec<int> ia;
	arg2intargs(ia, ce[0]);
	vec<IntVar*> iv;
	arg2intvarargs(iv, ce[1]);
	int_linear(ia, iv, irt, ce[2]->getInt(), getBoolVar(ce[3]));
}
void p_int_lin_CMP_imp(IntRelType irt, const ConExpr& ce, AST::Node* ann) {
	if (ce[3]->isBool()) {
		if (ce[3]->getBool()) {
			p_int_lin_CMP(irt, ce, ann);
		} else {
			p_int_lin_CMP(!irt, ce, ann);
		}
		return;
	}
	vec<int> ia;
	arg2intargs(ia, ce[0]);
	vec<IntVar*> iv;
	arg2intvarargs(iv, ce[1]);
	int_linear_imp(ia, iv, irt, ce[2]->getInt(), getBoolVar(ce[3]));
}
void p_int_lin_eq(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP(IRT_EQ, ce, ann); }
void p_int_lin_eq_reif(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP_reif(IRT_EQ, ce, ann); }
void p_int_lin_eq_imp(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP_imp(IRT_EQ, ce, ann); }
void p_int_lin_ne(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP(IRT_NE, ce, ann); }
void p_int_lin_ne_reif(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP_reif(IRT_NE, ce, ann); }
void p_int_lin_ne_imp(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP_imp(IRT_NE, ce, ann); }
void p_int_lin_le(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP(IRT_LE, ce, ann); }
void p_int_lin_le_reif(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP_reif(IRT_LE, ce, ann); }
void p_int_lin_le_imp(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP_imp(IRT_LE, ce, ann); }
void p_int_lin_lt(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP(IRT_LT, ce, ann); }
void p_int_lin_lt_reif(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP_reif(IRT_LT, ce, ann); }
void p_int_lin_lt_imp(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP_imp(IRT_LT, ce, ann); }
void p_int_lin_ge(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP(IRT_GE, ce, ann); }
void p_int_lin_ge_reif(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP_reif(IRT_GE, ce, ann); }
void p_int_lin_ge_imp(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP_imp(IRT_GE, ce, ann); }
void p_int_lin_gt(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP(IRT_GT, ce, ann); }
void p_int_lin_gt_reif(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP_reif(IRT_GT, ce, ann); }
void p_int_lin_gt_imp(const ConExpr& ce, AST::Node* ann) { p_int_lin_CMP_imp(IRT_GT, ce, ann); }

/* arithmetic constraints */

// can specialise
void p_int_plus(const ConExpr& ce, AST::Node* /*ann*/) {
	int_plus(getIntVar(ce[0]), getIntVar(ce[1]), getIntVar(ce[2]));
}

// can specialise
void p_int_minus(const ConExpr& ce, AST::Node* /*ann*/) {
	int_minus(getIntVar(ce[0]), getIntVar(ce[1]), getIntVar(ce[2]));
}

// can specialise
void p_int_pow(const ConExpr& ce, AST::Node* /*ann*/) {
	int_pow(getIntVar(ce[0]), getIntVar(ce[1]), getIntVar(ce[2]));
}
// can specialise
void p_int_times(const ConExpr& ce, AST::Node* /*ann*/) {
	int_times(getIntVar(ce[0]), getIntVar(ce[1]), getIntVar(ce[2]));
}
// can specialise?
void p_int_div(const ConExpr& ce, AST::Node* /*ann*/) {
	int_div(getIntVar(ce[0]), getIntVar(ce[1]), getIntVar(ce[2]));
}
void p_int_mod(const ConExpr& ce, AST::Node* /*ann*/) {
	int_mod(getIntVar(ce[0]), getIntVar(ce[1]), getIntVar(ce[2]));
}

void p_int_min(const ConExpr& ce, AST::Node* /*ann*/) {
	int_min(getIntVar(ce[0]), getIntVar(ce[1]), getIntVar(ce[2]));
}
void p_int_max(const ConExpr& ce, AST::Node* /*ann*/) {
	int_max(getIntVar(ce[0]), getIntVar(ce[1]), getIntVar(ce[2]));
}
void p_abs(const ConExpr& ce, AST::Node* /*ann*/) { int_abs(getIntVar(ce[0]), getIntVar(ce[1])); }
// can specialise
void p_int_negate(const ConExpr& ce, AST::Node* /*ann*/) {
	int_negate(getIntVar(ce[0]), getIntVar(ce[1]));
}
void p_range_size_fzn(const ConExpr& ce, AST::Node* /*ann*/) {
	range_size(getIntVar(ce[0]), getIntVar(ce[1]));
}

/* Boolean constraints */
void p_bool_CMP(int brt, const ConExpr& ce, AST::Node* /*ann*/, int sz) {
	const BoolView b1 = ce[0]->isBoolVar() ? getBoolVar(ce[0]) : bv_true;
	if (ce[0]->isBool()) {
		if (ce[0]->getBool()) {
			brt &= 0xaa;
			brt |= (brt >> 1);
		} else {
			brt &= 0x55;
			brt |= (brt << 1);
		}
	}
	const BoolView b2 = ce[1]->isBoolVar() ? getBoolVar(ce[1]) : bv_true;
	if (ce[1]->isBool()) {
		if (ce[1]->getBool()) {
			brt &= 0xcc;
			brt |= (brt >> 2);
		} else {
			brt &= 0x33;
			brt |= (brt << 2);
		}
	}
	if (sz == 2) {
		bool_rel(b1, (BoolRelType)brt, b2);
		return;
	}
	const BoolView b3 = ce[2]->isBoolVar() ? getBoolVar(ce[2]) : bv_true;
	if (ce[2]->isBool()) {
		if (ce[2]->getBool()) {
			brt &= 0xf0;
			brt |= (brt >> 4);
		} else {
			brt &= 0x0f;
			brt |= (brt << 4);
		}
	}
	bool_rel(b1, (BoolRelType)brt, b2, b3);
}

void p_bool_and(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_AND, ce, ann, 3); }
void p_bool_not(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_NOT, ce, ann, 2); }
void p_bool_or(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_OR, ce, ann, 3); }
void p_bool_xor(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_XOR, ce, ann, 3); }
void p_bool_eq(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_EQ, ce, ann, 2); }
void p_bool_eq_imp(const ConExpr& ce, AST::Node* ann) {
	if (ce[2]->isBool()) {
		if (ce[2]->getBool()) {
			p_bool_eq(ce, ann);
		}
		return;
	}
	vec<Lit> ps1;
	ps1.push(~s->bv[ce[2]->getBoolVar()]);
	if (ce[0]->isBoolVar()) {
		if (ce[1]->isBoolVar()) {
			ps1.push(~s->bv[ce[1]->getBoolVar()]);
			ps1.push(s->bv[ce[0]->getBoolVar()]);
			vec<Lit> ps2;
			ps2.push(~s->bv[ce[2]->getBoolVar()]);
			ps2.push(s->bv[ce[1]->getBoolVar()]);
			ps2.push(~s->bv[ce[0]->getBoolVar()]);
			sat.addClause(ps2);
		} else {
			if (ce[1]->getBool()) {
				ps1.push(s->bv[ce[0]->getBoolVar()]);
			} else {
				ps1.push(~s->bv[ce[0]->getBoolVar()]);
			}
		}
	} else {
		if (ce[0]->getBool()) {
			ps1.push(s->bv[ce[1]->getBoolVar()]);
		} else {
			ps1.push(~s->bv[ce[1]->getBoolVar()]);
		}
	}
	sat.addClause(ps1);
}
void p_bool_eq_reif(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_EQ_REIF, ce, ann, 3); }
void p_bool_ne(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_NE, ce, ann, 2); }
void p_bool_ne_reif(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_NE_REIF, ce, ann, 3); }
void p_bool_le(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_LE, ce, ann, 2); }
void p_bool_le_reif(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_LE_REIF, ce, ann, 3); }
void p_bool_lt(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_LT, ce, ann, 2); }
void p_bool_lt_reif(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_LT_REIF, ce, ann, 3); }
void p_bool_ge(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_GE, ce, ann, 2); }
void p_bool_ge_reif(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_GE_REIF, ce, ann, 3); }
void p_bool_gt(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_GT, ce, ann, 2); }
void p_bool_gt_reif(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_GT_REIF, ce, ann, 3); }
void p_bool_l_imp(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_L_IMPL, ce, ann, 3); }
void p_bool_r_imp(const ConExpr& ce, AST::Node* ann) { p_bool_CMP(BRT_R_IMPL, ce, ann, 3); }

void p_array_bool_and(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> bv;
	arg2BoolVarArgs(bv, ce[0]);
	array_bool_and(bv, getBoolVar(ce[1]));
}
void p_array_bool_or(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> bv;
	arg2BoolVarArgs(bv, ce[0]);
	array_bool_or(bv, getBoolVar(ce[1]));
}

// specialise?
void p_array_bool_clause(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> bvp;
	arg2BoolVarArgs(bvp, ce[0]);
	vec<BoolView> bvn;
	arg2BoolVarArgs(bvn, ce[1]);
	bool_clause(bvp, bvn);
}
// specialise?
void p_array_bool_clause_reif(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> bvp;
	arg2BoolVarArgs(bvp, ce[0]);
	vec<BoolView> bvn;
	arg2BoolVarArgs(bvn, ce[1]);
	const BoolView b0 = getBoolVar(ce[2]);
	array_bool_or(bvp, bvn, b0);
}

/* element constraints */
void p_array_int_element(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<int> ia;
	arg2intargs(ia, ce[1]);
	IntVar* sel = getIntVar(ce[0]);
	int_rel(sel, IRT_GE, 1);
	int_rel(sel, IRT_LE, ia.size());
	array_int_element(sel, ia, getIntVar(ce[2]), 1);
}
void p_array_var_int_element(const ConExpr& ce, AST::Node* ann) {
	vec<IntVar*> iv;
	arg2intvarargs(iv, ce[1]);
	IntVar* sel = getIntVar(ce[0]);
	int_rel(sel, IRT_GE, 1);
	int_rel(sel, IRT_LE, iv.size());
	if (ann2icl(ann) == CL_DOM) {
		array_var_int_element_dom(sel, iv, getIntVar(ce[2]), 1);
	} else {
		array_var_int_element_bound(sel, iv, getIntVar(ce[2]), 1);
	}
}
void p_array_var_int_element_imp(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> iv;
	arg2intvarargs(iv, ce[1]);
	IntVar* sel = getIntVar(ce[0]);
	const BoolView r = getBoolVar(ce[3]);
	array_var_int_element_bound_imp(r, sel, iv, getIntVar(ce[2]), 1);
}
void p_array_bool_element(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<bool> ba;
	arg2boolargs(ba, ce[1]);
	IntVar* sel = getIntVar(ce[0]);
	int_rel(sel, IRT_GE, 1);
	int_rel(sel, IRT_LE, ba.size());
	array_bool_element(sel, ba, getBoolVar(ce[2]), 1);
}
void p_array_var_bool_element(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> bv;
	arg2BoolVarArgs(bv, ce[1]);
	IntVar* sel = getIntVar(ce[0]);
	int_rel(sel, IRT_GE, 1);
	int_rel(sel, IRT_LE, bv.size());
	array_var_bool_element(sel, bv, getBoolVar(ce[2]), 1);
}

void p_set_in(const ConExpr& ce, AST::Node* /*unused*/) {
	if (ce[1]->isSetVar()) {
		CHUFFED_ERROR("Cannot handle set vars\n");
	}
	AST::SetLit* sl = ce[1]->getSet();
	if (ce[0]->isBoolVar()) {
		assert(sl->interval);
		const BoolView v = getBoolVar(ce[0]);
		if (sl->min >= 1) {
			if (!v.setVal(true)) {
				TL_FAIL();
			}
		}
		if (sl->max <= 0) {
			if (!v.setVal(false)) {
				TL_FAIL();
			}
		}
		return;
	}
	IntVar* v = getIntVar(ce[0]);
	if (sl->interval) {
		int_rel(v, IRT_GE, sl->min);
		int_rel(v, IRT_LE, sl->max);
	} else {
		vec<int> is(sl->s.size());
		for (unsigned int i = 0; i < sl->s.size(); i++) {
			is[i] = sl->s[i];
		}
		if (!v->allowSet(is)) {
			TL_FAIL();
		}
	}
}

void p_set_in_reif(const ConExpr& ce, AST::Node* /*unused*/) {
	if (ce[1]->isSetVar()) {
		CHUFFED_ERROR("Cannot handle set vars\n");
	}
	assert(ce[0]->isIntVar() || ce[0]->isInt());
	assert(ce[2]->isBoolVar());
	AST::SetLit* sl = ce[1]->getSet();
	IntVar* v = getIntVar(ce[0]);
	const BoolView r = getBoolVar(ce[2]);
	// TODO: Seems a bit wasteful to create new boolvars here
	auto add_reif_lbl = [](const BoolView& v, std::string&& label) {
		std::string const lbl = "(" + label + ")";
		boolVarString.emplace(v, label);
		litString.emplace(toInt(v.getLit(true)), lbl + "=true");
		litString.emplace(toInt(v.getLit(false)), lbl + "=false");
	};
	if (sl->interval) {
		const BoolView r1 = newBoolVar();
		int_rel_reif(v, IRT_GE, sl->min, r1);
		add_reif_lbl(r1, intVarString[v] + ">=" + std::to_string(sl->min));
		const BoolView r2 = newBoolVar();
		int_rel_reif(v, IRT_LE, sl->max, r2);
		add_reif_lbl(r2, intVarString[v] + "<=" + std::to_string(sl->min));
		bool_rel(r1, BRT_AND, r2, r);
	} else {
		vec<BoolView> rs;
		for (const int i : sl->s) {
			rs.push(newBoolVar());
			add_reif_lbl(rs.last(), intVarString[v] + "==" + std::to_string(i));
			int_rel_reif(v, IRT_EQ, i, rs.last());
		}
		array_bool_or(rs, r);
	}
}

/* coercion constraints */
void p_bool2int(const ConExpr& ce, AST::Node* /*ann*/) {
	bool2int(getBoolVar(ce[0]), getIntVar(ce[1]));
}

/* constraints from the standard library */

void p_all_different_int(const ConExpr& ce, AST::Node* ann) {
	vec<IntVar*> va;
	arg2intvarargs(va, ce[0]);
	all_different(va, ann2icl(ann));
}

void p_all_different_int_imp(const ConExpr& ce, AST::Node* ann) {
	vec<IntVar*> va;
	arg2intvarargs(va, ce[0]);
	assert(ce[1]->isBoolVar());
	const BoolView r = getBoolVar(ce[1]);
	all_different_imp(r, va, ann2icl(ann));
}

void p_inverse_offsets(const ConExpr& ce, AST::Node* ann) {
	vec<IntVar*> x;
	arg2intvarargs(x, ce[0]);
	vec<IntVar*> y;
	arg2intvarargs(y, ce[2]);
	inverse(x, y, ce[1]->getInt(), ce[3]->getInt(), ann2icl(ann));
}

void p_table_int(const ConExpr& ce, AST::Node* ann) {
	vec<IntVar*> x;
	arg2intvarargs(x, ce[0]);
	vec<int> tuples;
	arg2intargs(tuples, ce[1]);
	const int noOfVars = x.size();
	const int noOfTuples = tuples.size() / noOfVars;
	vec<vec<int> > ts;
	for (int i = 0; i < noOfTuples; i++) {
		ts.push();
		for (int j = 0; j < x.size(); j++) {
			ts.last().push(tuples[i * noOfVars + j]);
		}
	}
	if ((ann != nullptr) && (ann->hasAtom("mdd") || ann->hasCall("mdd"))) {
		mdd_table(x, ts, getMDDOpts(ann));
	} else {
		table(x, ts);
	}
}

void p_regular(const ConExpr& ce, AST::Node* ann) {
	vec<IntVar*> iv;
	arg2intvarargs(iv, ce[0]);
	const int q = ce[1]->getInt();
	const int s = ce[2]->getInt();
	vec<int> d_flat;
	arg2intargs(d_flat, ce[3]);
	const int q0 = ce[4]->getInt();

	assert(d_flat.size() == q * s);

	vec<vec<int> > d;
	for (int i = 0; i < q; i++) {
		d.push();
		for (int j = 0; j < s; j++) {
			d.last().push(d_flat[i * s + j]);
		}
	}

	// Final states
	AST::SetLit* sl = ce[5]->getSet();
	vec<int> f;
	if (sl->interval) {
		for (int i = sl->min; i <= sl->max; i++) {
			f.push(i);
		}
	} else {
		for (const int& i : sl->s) {
			f.push(i);
		}
	}

	if ((ann != nullptr) && ann->hasAtom("mdd")) {
		mdd_regular(iv, q, s, d, q0, f, true, getMDDOpts(ann));
	} else {
		regular(iv, q, s, d, q0, f);
	}
}

void p_cost_regular(const ConExpr& ce, AST::Node* ann) {
	vec<IntVar*> iv;
	arg2intvarargs(iv, ce[0]);
	const int q = ce[1]->getInt();
	const int s = ce[2]->getInt();
	vec<int> d_flat;
	arg2intargs(d_flat, ce[3]);
	vec<int> w_flat;
	arg2intargs(w_flat, ce[4]);
	const int q0 = ce[5]->getInt();

	assert(d_flat.size() == q * s);

	vec<vec<int> > d;
	vec<vec<int> > w;
	// State 0 is garbage
	d.push();
	for (int j = 0; j <= s; j++) {
		d.last().push(0);
		w.last().push(0);
	}
	// Fill in the remaining transitions
	for (int i = 0; i < q; i++) {
		d.push();
		w.push();
		// Values start from 1, so [x = 0] goes to garbage.
		d.last().push(0);
		w.last().push(0);
		for (int j = 0; j < s; j++) {
			d.last().push(d_flat[i * s + j]);
			w.last().push(w_flat[i * s + j]);
		}
	}

	// Final states
	AST::SetLit* sl = ce[6]->getSet();
	vec<int> f;
	if (sl->interval) {
		for (int i = sl->min; i <= sl->max; i++) {
			f.push(i);
		}
	} else {
		for (const int& i : sl->s) {
			f.push(i);
		}
	}

	IntVar* cost = getIntVar(ce[7]);

	wmdd_cost_regular(iv, q + 1, s + 1, d, w, q0, f, cost, getMDDOpts(ann));
}

void p_disjunctive(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> s;
	arg2intvarargs(s, ce[0]);
	vec<int> d;
	arg2intargs(d, ce[1]);
	disjunctive(s, d);
}

void p_cumulative(const ConExpr& ce, AST::Node* ann) {
	vec<IntVar*> s;
	arg2intvarargs(s, ce[0]);
	vec<int> d;
	arg2intargs(d, ce[1]);
	vec<int> p;
	arg2intargs(p, ce[2]);
	const int limit = ce[3]->getInt();
	const std::list<std::string> opt = getCumulativeOptions(ann);
	cumulative(s, d, p, limit, opt);
}

void p_cumulative2(const ConExpr& ce, AST::Node* ann) {
	vec<IntVar*> s;
	arg2intvarargs(s, ce[0]);
	vec<IntVar*> d;
	arg2intvarargs(d, ce[1]);
	vec<IntVar*> r;
	arg2intvarargs(r, ce[2]);
	const std::list<std::string> opt = getCumulativeOptions(ann);
	cumulative2(s, d, r, getIntVar(ce[3]), opt);
}

void p_cumulative_cal(const ConExpr& ce, AST::Node* ann) {
	vec<IntVar*> s;
	arg2intvarargs(s, ce[0]);
	vec<IntVar*> d;
	arg2intvarargs(d, ce[1]);
	vec<IntVar*> p;
	arg2intvarargs(p, ce[2]);

	const int index1 = ce[4]->getInt();
	const int index2 = ce[5]->getInt();

	vec<int> cal_in;
	arg2intargs(cal_in, ce[6]);

	vec<vec<int> > cal;
	for (int i = 0; i < index1; i++) {
		cal.push();
		for (int j = 0; j < index2; j++) {
			cal.last().push(cal_in[i * index2 + j]);
		}
	}

	vec<int> taskCal;
	arg2intargs(taskCal, ce[7]);
	const int rho = ce[8]->getInt();
	const int resCal = ce[9]->getInt();
	const std::list<std::string> opt = getCumulativeOptions(ann);
	cumulative_cal(s, d, p, getIntVar(ce[3]), cal, taskCal, rho, resCal, opt);
}

void p_circuit(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> x;
	arg2intvarargs(x, ce[0]);
	const int index_offset = ce[1]->getInt();
	circuit(x, index_offset);
}

void p_subcircuit(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> x;
	arg2intvarargs(x, ce[0]);
	const int index_offset = ce[1]->getInt();
	subcircuit(x, index_offset);
}

void p_minimum(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> iv;
	arg2intvarargs(iv, ce[1]);
	minimum(iv, getIntVar(ce[0]));
}

void p_maximum(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> iv;
	arg2intvarargs(iv, ce[1]);
	maximum(iv, getIntVar(ce[0]));
}

void p_bool_arg_max(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> bv;
	arg2BoolVarArgs(bv, ce[0]);
	bool_arg_max(bv, ce[1]->getInt(), getIntVar(ce[2]));
}

void p_lex_less(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> iv0;
	arg2intvarargs(iv0, ce[0]);
	vec<IntVar*> iv1;
	arg2intvarargs(iv1, ce[1]);
	lex(iv0, iv1, true);
}

void p_lex_lesseq(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> iv0;
	arg2intvarargs(iv0, ce[0]);
	vec<IntVar*> iv1;
	arg2intvarargs(iv1, ce[1]);
	lex(iv0, iv1, false);
}

void p_edit_distance(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<int> insertion_costs;
	arg2intargs(insertion_costs, ce[1]);
	vec<int> deletion_costs;
	arg2intargs(deletion_costs, ce[2]);
	vec<int> substitution_costs;
	arg2intargs(substitution_costs, ce[3]);
	vec<IntVar*> seq1;
	arg2intvarargs(seq1, ce[4]);
	vec<IntVar*> seq2;
	arg2intvarargs(seq2, ce[5]);
	edit_distance(ce[0]->getInt(), insertion_costs, deletion_costs, substitution_costs, seq1, seq2,
								getIntVar(ce[6]));
}

void var_sym(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> iv0;
	arg2intvarargs(iv0, ce[0]);
	var_sym_ldsb(iv0);
}

void val_sym(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> iv0;
	arg2intvarargs(iv0, ce[0]);
	val_sym_ldsb(iv0, ce[1]->getInt(), ce[2]->getInt());
}

void var_seq_sym(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> iv0;
	arg2intvarargs(iv0, ce[2]);
	var_seq_sym_ldsb(ce[0]->getInt(), ce[1]->getInt(), iv0);
}

void val_seq_sym(const ConExpr& /*ce*/, AST::Node* /*ann*/) { NOT_SUPPORTED; }

void val_prec(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> iv0;
	arg2intvarargs(iv0, ce[2]);
	value_precede_int(ce[0]->getInt(), ce[1]->getInt(), iv0);
}
void val_prec_seq(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<IntVar*> iv0;
	arg2intvarargs(iv0, ce[0]);
	value_precede_seq(iv0);
}

void p_bool_sum_CMP(IntRelType irt, const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> bv;
	arg2BoolVarArgs(bv, ce[0]);
	bool_linear(bv, irt, getIntVar(ce[1]));
}
void p_bool_sum_eq(const ConExpr& ce, AST::Node* ann) { p_bool_sum_CMP(IRT_EQ, ce, ann); }
void p_bool_sum_le(const ConExpr& ce, AST::Node* ann) { p_bool_sum_CMP(IRT_LE, ce, ann); }
void p_bool_sum_lt(const ConExpr& ce, AST::Node* ann) { p_bool_sum_CMP(IRT_LT, ce, ann); }
void p_bool_sum_ge(const ConExpr& ce, AST::Node* ann) { p_bool_sum_CMP(IRT_GE, ce, ann); }
void p_bool_sum_gt(const ConExpr& ce, AST::Node* ann) { p_bool_sum_CMP(IRT_GT, ce, ann); }

/*
		void p_bool_lin_CMP(IntRelType irt, const ConExpr& ce, AST::Node* ann) {
			vec<int> ia; arg2intargs(ia, ce[0]);
			vec<BoolVar*> bv; arg2BoolVarArgs(bv, ce[1]);
			if (ce[2]->isIntVar())
				bool_linear(bv, irt, s->iv[ce[2]->getIntVar()]);
			else
				bool_linear(bv, irt, ce[2]->getInt());
		}
		void p_bool_lin_CMP_reif(IntRelType irt, const ConExpr& ce, AST::Node* ann) {
			if (ce[2]->isBool()) {
				if (ce[2]->getBool()) {
					p_bool_lin_CMP(irt, ce, ann);
				} else {
					p_bool_lin_CMP(neg(irt), ce, ann);
				}
				return;
			}
			IntArgs ia = arg2intargs(ce[0]);
			vec<BoolView> iv = arg2BoolVarArgs(ce[1]);
			if (ce[2]->isIntVar())
				linear(ia, iv, irt, iv[ce[2]->getIntVar()], getBoolVar(ce[3]),
							 ann2icl(ann));
			else
				linear(ia, iv, irt, ce[2]->getInt(), getBoolVar(ce[3]),
							 ann2icl(ann));
		}
		void p_bool_lin_eq(const ConExpr& ce, AST::Node* ann) {
			p_bool_lin_CMP(IRT_EQ, ce, ann);
		}
		void p_bool_lin_eq_reif(const ConExpr& ce, AST::Node* ann) {
			p_bool_lin_CMP_reif(IRT_EQ, ce, ann);
		}
		void p_bool_lin_ne(const ConExpr& ce, AST::Node* ann) {
			p_bool_lin_CMP(IRT_NE, ce, ann);
		}
		void p_bool_lin_ne_reif(const ConExpr& ce, AST::Node* ann) {
			p_bool_lin_CMP_reif(IRT_NE, ce, ann);
		}
		void p_bool_lin_le(const ConExpr& ce, AST::Node* ann) {
			p_bool_lin_CMP(IRT_LE, ce, ann);
		}
		void p_bool_lin_le_reif(const ConExpr& ce, AST::Node* ann) {
			p_bool_lin_CMP_reif(IRT_LE, ce, ann);
		}
		void p_bool_lin_lt(const ConExpr& ce, AST::Node* ann) {
			p_bool_lin_CMP(IRT_LT, ce, ann);
		}
		void p_bool_lin_lt_reif(const ConExpr& ce, AST::Node* ann) {
			p_bool_lin_CMP_reif(IRT_LT, ce, ann);
		}
		void p_bool_lin_ge(const ConExpr& ce, AST::Node* ann) {
			p_bool_lin_CMP(IRT_GE, ce, ann);
		}
		void p_bool_lin_ge_reif(const ConExpr& ce, AST::Node* ann) {
			p_bool_lin_CMP_reif(IRT_GE, ce, ann);
		}
		void p_bool_lin_gt(const ConExpr& ce, AST::Node* ann) {
			p_bool_lin_CMP(IRT_GT, ce, ann);
		}
		void p_bool_lin_gt_reif(const ConExpr& ce, AST::Node* ann) {
			p_bool_lin_CMP_reif(IRT_GT, ce, ann);
		}
*/

/*

		void p_distinctOffset(const ConExpr& ce, AST::Node* ann) {
			vec<IntVar*> va = arg2intvarargs(ce[1]);
			AST::Array* offs = ce.args->a[0]->getArray();
			IntArgs oa(offs->a.size());
			for (int i=offs->a.size(); i--; ) {
				oa[i] = offs->a[i]->getInt();
			}
			distinct(oa, va, ann2icl(ann));
		}

		void p_count(const ConExpr& ce, AST::Node* ann) {
			vec<IntVar*> iv = arg2intvarargs(ce[0]);
			if (!ce[1]->isIntVar()) {
				if (!ce[2]->isIntVar()) {
					count(iv, ce[1]->getInt(), IRT_EQ, ce[2]->getInt(),
								ann2icl(ann));
				} else {
					count(iv, ce[1]->getInt(), IRT_EQ, getIntVar(ce[2]),
								ann2icl(ann));
				}
			} else if (!ce[2]->isIntVar()) {
				count(iv, getIntVar(ce[1]), IRT_EQ, ce[2]->getInt(),
							ann2icl(ann));
			} else {
				count(iv, getIntVar(ce[1]), IRT_EQ, getIntVar(ce[2]),
							ann2icl(ann));
			}
		}

		void count_rel(IntRelType irt,
									 FlatZincSpace& s, const ConExpr& ce, AST::Node* ann) {
			vec<IntVar*> iv = arg2intvarargs(ce[1]);
			count(iv, ce[2]->getInt(), irt, ce[0]->getInt(), ann2icl(ann));
		}

		void p_at_most(const ConExpr& ce, AST::Node* ann) {
			count_rel(IRT_LE, s, ce, ann);
		}

		void p_at_least(const ConExpr& ce, AST::Node* ann) {
			count_rel(IRT_GE, s, ce, ann);
		}

		void p_global_cardinality(const ConExpr& ce,
															AST::Node* ann) {
			vec<IntVar*> iv0 = arg2intvarargs(ce[0]);
			vec<IntVar*> iv1 = arg2intvarargs(ce[1]);
			int cmin = ce[2]->getInt();
			if (cmin == 0) {
				count(iv0, iv1, ann2icl(ann));
			} else {
				IntArgs values(iv1.size());
				for (int i=values.size(); i--;)
					values[i] = i+cmin;
				count(iv0, iv1, values, ann2icl(ann));
			}
		}

		void
		p_sort(const ConExpr& ce, AST::Node* ann) {
			vec<IntVar*> x = arg2intvarargs(ce[0]);
			vec<IntVar*> y = arg2intvarargs(ce[1]);
			vec<IntVar*> xy(x.size()+y.size());
			for (int i=x.size(); i--;)
				xy[i] = x[i];
			for (int i=y.size(); i--;)
				xy[i+x.size()] = y[i];
			unshare(xy);
			for (int i=x.size(); i--;)
				x[i] = xy[i];
			for (int i=y.size(); i--;)
				y[i] = xy[i+x.size()];
			sorted(x, y, ann2icl(ann));
		}

		void
		p_increasing_int(const ConExpr& ce, AST::Node* ann) {
			vec<IntVar*> x = arg2intvarargs(ce[0]);
			rel(s,x,IRT_LE,ann2icl(ann));
		}

		void
		p_increasing_bool(const ConExpr& ce, AST::Node* ann) {
			vec<BoolView> x = arg2BoolVarArgs(ce[0]);
			rel(s,x,IRT_LE,ann2icl(ann));
		}

		void
		p_table_bool(const ConExpr& ce, AST::Node* ann) {
			vec<BoolView> x = arg2BoolVarArgs(ce[0]);
			IntArgs tuples = arg2boolargs(ce[1]);
			int noOfVars   = x.size();
			int noOfTuples = tuples.size()/noOfVars;
			TupleSet ts;
			for (int i=0; i<noOfTuples; i++) {
				IntArgs t(noOfVars);
				for (int j=0; j<x.size(); j++) {
					t[j] = tuples[i*noOfVars+j];
				}
				ts.add(t);
			}
			ts.finalize();
			extensional(s,x,ts,EPK_DEF,ann2icl(ann));
		}

		void p_cumulatives(const ConExpr& ce,
											AST::Node* ann) {
			vec<IntVar*> start = arg2intvarargs(ce[0]);
			vec<IntVar*> duration = arg2intvarargs(ce[1]);
			vec<IntVar*> height = arg2intvarargs(ce[2]);
			int n = start.size();
			IntVar bound = getIntVar(ce[3]);

			if (bound.assigned()) {
				IntArgs machine(n);
				for (int i = n; i--; ) machine[i] = 0;
				IntArgs limit(1, bound.val());
				vec<IntVar*> end(n);
				for (int i = n; i--; ) end[i] = IntVar(0, Int::Limits::max);
				cumulatives(machine, start, duration, end, height, limit, true,
										ann2icl(ann));
			} else {
				int min = Gecode::Int::Limits::max;
				int max = Gecode::Int::Limits::min;
				vec<IntVar*> end(start.size());
				for (int i = start.size(); i--; ) {
					min = std::min(min, start[i].min());
					max = std::max(max, start[i].max() + duration[i].max());
					end[i] = post(start[i] + duration[i]);
				}
				for (int time = min; time < max; ++time) {
					vec<IntVar*> x(start.size());
					for (int i = start.size(); i--; ) {
						IntVar overlaps = channel(post((~(start[i] <= time) &&
																									~(time < end[i]))));
						x[i] = mult(overlaps, height[i]);
					}
					linear(x, IRT_LE, bound);
				}
			}
		}
*/

void p_tree(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[0]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[1]);
	vec<bool> ad_flat;
	arg2boolargs(ad_flat, ce[2]);
	assert(ad_flat.size() == vs.size() * es.size());
	vec<vec<int> > ad;
	for (int i = 0; i < vs.size(); i++) {
		ad.push(vec<int>());
		for (int j = 0; j < es.size(); j++) {
			if (ad_flat[i * es.size() + j]) {
				ad[i].push(j);
			}
		}
	}
	vec<int> en_flat;
	try {
		vec<bool> en_flat_b;
		arg2boolargs(en_flat_b, ce[3]);
		for (int i = 0; i < en_flat_b.size(); i++) {
			en_flat.push(en_flat_b[i] ? 1 : 0);
		}
	} catch (FlatZinc::AST::TypeError& e) {
		arg2intargs(en_flat, ce[3]);
	}

	vec<vec<int> > en;

	if (en_flat.size() == es.size() * vs.size()) {
		// Old format!
		for (int i = 0; i < es.size(); i++) {
			en.push(vec<int>());
			const int check = 0;
			for (int j = 0; j < vs.size(); j++) {
				if (en_flat[i * vs.size() + j] != 0) {
					en[i].push(j);
				}
			}
			if (en[i].size() == 1) {  // Self loops
				en[i].push(en[i][0]);
			}
		}
	} else if (en_flat.size() == es.size() * 2) {
		// New format
		for (int i = 0; i < es.size(); i++) {
			en.push(vec<int>());
			// The -1 is becaise indexes in MZ start at 1
			en[i].push(en_flat[i] - 1);
			en[i].push(en_flat[es.size() + i] - 1);
		}
	}
	tree(vs, es, ad, en);
}

void p_tree_new(const ConExpr& ce, AST::Node* /*ann*/) {
	const int nb_nodes = ce[0]->getInt();
	const int nb_edges = ce[1]->getInt();
	vec<int> from;
	arg2intargs(from, ce[2]);
	vec<int> to;
	arg2intargs(to, ce[3]);
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[4]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[5]);

	vec<vec<int> > en;
	vec<vec<int> > ad;
	for (int i = 0; i < nb_nodes; i++) {
		ad.push(vec<int>());
	}
	for (int i = 0; i < nb_edges; i++) {
		en.push(vec<int>());
		// The -1 is because indexes in MZ start at 1
		const int u = from[i] - 1;
		const int v = to[i] - 1;
		en[i].push(u);
		en[i].push(v);
		ad[u].push(i);
		ad[v].push(i);
	}
	tree(vs, es, ad, en);
}

void p_connected(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[0]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[1]);
	vec<bool> ad_flat;
	arg2boolargs(ad_flat, ce[2]);
	assert(ad_flat.size() == vs.size() * es.size());
	vec<vec<int> > ad;
	for (int i = 0; i < vs.size(); i++) {
		ad.push(vec<int>());
		for (int j = 0; j < es.size(); j++) {
			if (ad_flat[i * es.size() + j]) {
				ad[i].push(j);
			}
		}
	}
	vec<int> en_flat;
	try {
		vec<bool> en_flat_b;
		arg2boolargs(en_flat_b, ce[3]);
		for (int i = 0; i < en_flat_b.size(); i++) {
			en_flat.push(en_flat_b[i] ? 1 : 0);
		}
	} catch (FlatZinc::AST::TypeError& e) {
		arg2intargs(en_flat, ce[3]);
	}

	vec<vec<int> > en;

	if (en_flat.size() == es.size() * vs.size()) {
		// Old format!
		for (int i = 0; i < es.size(); i++) {
			en.push(vec<int>());
			const int check = 0;
			for (int j = 0; j < vs.size(); j++) {
				if (en_flat[i * vs.size() + j] != 0) {
					en[i].push(j);
				}
			}
			if (en[i].size() == 1) {  // Self loops
				en[i].push(en[i][0]);
			}
		}
	} else if (en_flat.size() == es.size() * 2) {
		// New format
		for (int i = 0; i < es.size(); i++) {
			en.push(vec<int>());
			// The -1 is becaise indexes in MZ start at 1
			en[i].push(en_flat[i] - 1);
			en[i].push(en_flat[es.size() + i] - 1);
		}
	}
	connected(vs, es, ad, en);
}

void p_connected_new(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[2]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[3]);
	const int nb_nodes = vs.size();
	const int nb_edges = es.size();
	vec<int> from;
	arg2intargs(from, ce[0]);
	vec<int> to;
	arg2intargs(to, ce[1]);

	vec<vec<int> > en;
	vec<vec<int> > ad;
	for (int i = 0; i < nb_nodes; i++) {
		ad.push(vec<int>());
	}
	for (int i = 0; i < nb_edges; i++) {
		en.push(vec<int>());
		// The -1 is because indexes in MZ start at 1
		const int u = from[i] - 1;
		const int v = to[i] - 1;
		en[i].push(u);
		en[i].push(v);
		ad[u].push(i);
		ad[v].push(i);
	}
	connected(vs, es, ad, en);
}

void p_steiner_tree(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[0]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[1]);
	IntVar* w = getIntVar(ce[4]);
	vec<int> ws;
	arg2intargs(ws, ce[5]);

	vec<bool> ad_flat;
	arg2boolargs(ad_flat, ce[2]);
	assert(ad_flat.size() == vs.size() * es.size());
	vec<vec<int> > ad;
	for (int i = 0; i < vs.size(); i++) {
		ad.push(vec<int>());
		for (int j = 0; j < es.size(); j++) {
			if (ad_flat[i * es.size() + j]) {
				ad[i].push(j);
			}
		}
	}

	vec<int> en_flat;
	try {
		vec<bool> en_flat_b;
		arg2boolargs(en_flat_b, ce[3]);
		for (int i = 0; i < en_flat_b.size(); i++) {
			en_flat.push(en_flat_b[i] ? 1 : 0);
		}
	} catch (FlatZinc::AST::TypeError& e) {
		arg2intargs(en_flat, ce[3]);
	}

	vec<vec<int> > en;

	if (en_flat.size() == es.size() * vs.size()) {
		// Old format!
		for (int i = 0; i < es.size(); i++) {
			en.push(vec<int>());
			const int check = 0;
			for (int j = 0; j < vs.size(); j++) {
				if (en_flat[i * vs.size() + j] != 0) {
					en[i].push(j);
				}
			}
			if (en[i].size() == 1) {  // Self loops
				en[i].push(en[i][0]);
			}
		}
	} else if (en_flat.size() == es.size() * 2) {
		// New format
		for (int i = 0; i < es.size(); i++) {
			en.push(vec<int>());
			// The -1 is becaise indexes in MZ start at 1
			en[i].push(en_flat[i] - 1);
			en[i].push(en_flat[es.size() + i] - 1);
		}
	}

	steiner_tree(vs, es, ad, en, w, ws);
}

void p_steiner_tree_new(const ConExpr& ce, AST::Node* /*ann*/) {
	const int nb_nodes = ce[0]->getInt();
	const int nb_edges = ce[1]->getInt();
	vec<int> from;
	arg2intargs(from, ce[2]);
	vec<int> to;
	arg2intargs(to, ce[3]);
	vec<int> ws;
	arg2intargs(ws, ce[4]);
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[5]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[6]);
	IntVar* w = getIntVar(ce[7]);

	vec<vec<int> > en;
	vec<vec<int> > ad;
	for (int i = 0; i < nb_nodes; i++) {
		ad.push(vec<int>());
	}
	for (int i = 0; i < nb_edges; i++) {
		en.push(vec<int>());
		// The -1 is because indexes in MZ start at 1
		const int u = from[i] - 1;
		const int v = to[i] - 1;
		en[i].push(u);
		en[i].push(v);
		ad[u].push(i);
		ad[v].push(i);
	}
	steiner_tree(vs, es, ad, en, w, ws);
}

void p_mst(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[0]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[1]);

	vec<bool> ad_flat;
	arg2boolargs(ad_flat, ce[2]);
	assert(ad_flat.size() == vs.size() * es.size());
	vec<vec<int> > ad;
	for (int i = 0; i < vs.size(); i++) {
		ad.push(vec<int>());
		for (int j = 0; j < es.size(); j++) {
			if (ad_flat[i * es.size() + j]) {
				ad[i].push(j);
			}
		}
	}
	vec<int> en_flat;
	try {
		vec<bool> en_flat_b;
		arg2boolargs(en_flat_b, ce[3]);
		for (int i = 0; i < en_flat_b.size(); i++) {
			en_flat.push(en_flat_b[i] ? 1 : 0);
		}
	} catch (FlatZinc::AST::TypeError& e) {
		arg2intargs(en_flat, ce[3]);
	}

	vec<vec<int> > en;

	if (en_flat.size() == es.size() * vs.size()) {
		// Old format!
		for (int i = 0; i < es.size(); i++) {
			en.push(vec<int>());
			const int check = 0;
			for (int j = 0; j < vs.size(); j++) {
				if (en_flat[i * vs.size() + j] != 0) {
					en[i].push(j);
				}
			}
			if (en[i].size() == 1) {  // Self loops
				en[i].push(en[i][0]);
			}
		}
	} else if (en_flat.size() == es.size() * 2) {
		// New format
		for (int i = 0; i < es.size(); i++) {
			en.push(vec<int>());
			// The -1 is becaise indexes in MZ start at 1
			en[i].push(en_flat[i] - 1);
			en[i].push(en_flat[es.size() + i] - 1);
		}
	}

	IntVar* w = getIntVar(ce[4]);
	vec<int> ws;
	arg2intargs(ws, ce[5]);
	mst(vs, es, ad, en, w, ws);
}

void p_mst_new(const ConExpr& ce, AST::Node* /*ann*/) {
	const int nb_nodes = ce[0]->getInt();
	const int nb_edges = ce[1]->getInt();
	vec<int> from;
	arg2intargs(from, ce[2]);
	vec<int> to;
	arg2intargs(to, ce[3]);
	vec<int> ws;
	arg2intargs(to, ce[4]);
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[5]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[6]);
	IntVar* w = getIntVar(ce[7]);

	vec<vec<int> > en;
	vec<vec<int> > ad;
	for (int i = 0; i < nb_nodes; i++) {
		ad.push(vec<int>());
	}
	for (int i = 0; i < nb_edges; i++) {
		en.push(vec<int>());
		// The -1 is because indexes in MZ start at 1
		const int u = from[i] - 1;
		const int v = to[i] - 1;
		en[i].push(u);
		en[i].push(v);
		ad[u].push(i);
		ad[v].push(i);
	}
	mst(vs, es, ad, en, w, ws);
}

void p_dtree(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[0]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[1]);
	const int from = ce[2]->getInt() - 1;

	vec<bool> in_flat;
	arg2boolargs(in_flat, ce[3]);
	assert(in_flat.size() == vs.size() * es.size());
	vec<vec<int> > in;
	for (int i = 0; i < vs.size(); i++) {
		in.push(vec<int>());
		for (int j = 0; j < es.size(); j++) {
			if (in_flat[i * es.size() + j]) {
				in[i].push(j);
			}
		}
	}
	vec<bool> ou_flat;
	arg2boolargs(ou_flat, ce[4]);
	assert(ou_flat.size() == vs.size() * es.size());
	vec<vec<int> > ou;
	for (int i = 0; i < vs.size(); i++) {
		ou.push(vec<int>());
		for (int j = 0; j < es.size(); j++) {
			if (ou_flat[i * es.size() + j]) {
				ou[i].push(j);
			}
		}
	}
	vec<int> en_flat;
	arg2intargs(en_flat, ce[5]);
	// assert(en_flat.size() == es.size()*vs.size());
	assert(en_flat.size() == es.size() * 2);
	vec<vec<int> > en;
	for (int i = 0; i < es.size(); i++) {
		en.push(vec<int>());
		// The -1 is because indexes in MZ start at 1
		en[i].push(en_flat[i] - 1);
		en[i].push(en_flat[es.size() + i] - 1);
	}

	dtree(from, vs, es, in, ou, en);
}

void p_dtree_new(const ConExpr& ce, AST::Node* /*ann*/) {
	const int nb_nodes = ce[0]->getInt();
	const int nb_edges = ce[1]->getInt();
	vec<int> from;
	arg2intargs(from, ce[2]);
	vec<int> to;
	arg2intargs(to, ce[3]);
	const int root = ce[4]->getInt();
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[5]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[6]);

	vec<vec<int> > en;
	vec<vec<int> > in;
	vec<vec<int> > ou;
	for (int i = 0; i < nb_nodes; i++) {
		in.push(vec<int>());
		ou.push(vec<int>());
	}
	for (int i = 0; i < nb_edges; i++) {
		en.push(vec<int>());
		// The -1 is because indexes in MZ start at 1
		const int u = from[i] - 1;
		const int v = to[i] - 1;
		en[i].push(u);
		en[i].push(v);
		ou[u].push(i);
		in[v].push(i);
	}
	dtree(root, vs, es, in, ou, en);
}

void p_dag(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[0]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[1]);
	const int from = ce[2]->getInt() - 1;

	vec<bool> in_flat;
	arg2boolargs(in_flat, ce[3]);
	assert(in_flat.size() == vs.size() * es.size());
	vec<vec<int> > in;
	for (int i = 0; i < vs.size(); i++) {
		in.push(vec<int>());
		for (int j = 0; j < es.size(); j++) {
			if (in_flat[i * es.size() + j]) {
				in[i].push(j);
			}
		}
	}
	vec<bool> ou_flat;
	arg2boolargs(ou_flat, ce[4]);
	assert(ou_flat.size() == vs.size() * es.size());
	vec<vec<int> > ou;
	for (int i = 0; i < vs.size(); i++) {
		ou.push(vec<int>());
		for (int j = 0; j < es.size(); j++) {
			if (ou_flat[i * es.size() + j]) {
				ou[i].push(j);
			}
		}
	}
	vec<int> en_flat;
	arg2intargs(en_flat, ce[5]);
	// assert(en_flat.size() == es.size()*vs.size());
	assert(en_flat.size() == es.size() * 2);
	vec<vec<int> > en;
	for (int i = 0; i < es.size(); i++) {
		en.push(vec<int>());
		// The -1 is because indexes in MZ start at 1
		en[i].push(en_flat[i] - 1);
		en[i].push(en_flat[es.size() + i] - 1);
	}

	dag(from, vs, es, in, ou, en);
}

void p_dag_new(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<int> from;
	arg2intargs(from, ce[0]);
	vec<int> to;
	arg2intargs(to, ce[1]);
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[2]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[3]);
	int nb_nodes = vs.size();

	const int extra = nb_nodes;  // Extra node with edges to everyone
	vs.push(bv_true);
	vec<BoolView> new_edges;
	for (int i = 0; i < nb_nodes; i++) {
		from.push(extra + 1);
		to.push(i + 1);
		const BoolView new_edge = newBoolVar();
		es.push(new_edge);
		new_edges.push(new_edge);
	}

	vec<vec<int> > en;
	vec<vec<int> > in;
	vec<vec<int> > ou;
	nb_nodes = vs.size();
	for (int i = 0; i < nb_nodes; i++) {
		in.push(vec<int>());
		ou.push(vec<int>());
	}
	const int nb_edges = es.size();
	for (int i = 0; i < nb_edges; i++) {
		en.push(vec<int>());
		// The -1 is because indexes in MZ start at 1
		const int u = from[i] - 1;
		const int v = to[i] - 1;
		en[i].push(u);
		en[i].push(v);
		ou[u].push(i);
		in[v].push(i);
	}
	bool_linear(new_edges, IRT_LE, getConstant(1));
	dag(extra, vs, es, in, ou, en);
}

void p_path(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[0]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[1]);
	const int from = ce[2]->getInt() - 1;
	const int to = ce[3]->getInt() - 1;

	vec<bool> in_flat;
	arg2boolargs(in_flat, ce[4]);
	assert(in_flat.size() == vs.size() * es.size());
	vec<vec<int> > in;
	for (int i = 0; i < vs.size(); i++) {
		in.push(vec<int>());
		for (int j = 0; j < es.size(); j++) {
			if (in_flat[i * es.size() + j]) {
				in[i].push(j);
			}
		}
	}
	vec<bool> ou_flat;
	arg2boolargs(ou_flat, ce[5]);
	assert(ou_flat.size() == vs.size() * es.size());
	vec<vec<int> > ou;
	for (int i = 0; i < vs.size(); i++) {
		ou.push(vec<int>());
		for (int j = 0; j < es.size(); j++) {
			if (ou_flat[i * es.size() + j]) {
				ou[i].push(j);
			}
		}
	}
	vec<int> en_flat;
	arg2intargs(en_flat, ce[6]);
	// assert(en_flat.size() == es.size()*vs.size());
	assert(en_flat.size() == es.size() * 2);
	vec<vec<int> > en;
	for (int i = 0; i < es.size(); i++) {
		en.push(vec<int>());
		// The -1 is becaise indexes in MZ start at 1
		en[i].push(en_flat[i] - 1);
		en[i].push(en_flat[es.size() + i] - 1);
	}

	path(from, to, vs, es, in, ou, en);
}

void p_path_new(const ConExpr& ce, AST::Node* /*ann*/) {
	const int nb_nodes = ce[0]->getInt();
	const int nb_edges = ce[1]->getInt();
	vec<int> from;
	arg2intargs(from, ce[2]);
	vec<int> to;
	arg2intargs(to, ce[3]);
	const int s = ce[4]->getInt() - 1;
	const int t = ce[5]->getInt() - 1;
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[6]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[7]);

	vec<vec<int> > en;
	vec<vec<int> > in;
	vec<vec<int> > ou;
	for (int i = 0; i < nb_nodes; i++) {
		in.push(vec<int>());
		ou.push(vec<int>());
	}
	for (int i = 0; i < nb_edges; i++) {
		en.push(vec<int>());
		// The -1 is because indexes in MZ start at 1
		const int u = from[i] - 1;
		const int v = to[i] - 1;
		en[i].push(u);
		en[i].push(v);
		ou[u].push(i);
		in[v].push(i);
	}
	path(s, t, vs, es, in, ou, en);
}

void p_bounded_path(const ConExpr& ce, AST::Node* /*ann*/) {
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[0]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[1]);

	const int from = ce[2]->getInt() - 1;
	const int to = ce[3]->getInt() - 1;

	vec<bool> in_flat;
	arg2boolargs(in_flat, ce[4]);
	assert(in_flat.size() == vs.size() * es.size());
	vec<vec<int> > in;
	for (int i = 0; i < vs.size(); i++) {
		in.push(vec<int>());
		for (int j = 0; j < es.size(); j++) {
			if (in_flat[i * es.size() + j]) {
				in[i].push(j);
			}
		}
	}
	vec<bool> ou_flat;
	arg2boolargs(ou_flat, ce[5]);
	assert(ou_flat.size() == vs.size() * es.size());
	vec<vec<int> > ou;
	for (int i = 0; i < vs.size(); i++) {
		ou.push(vec<int>());
		for (int j = 0; j < es.size(); j++) {
			if (ou_flat[i * es.size() + j]) {
				ou[i].push(j);
			}
		}
	}
	std::cout << '\n';
	vec<int> en_flat;
	arg2intargs(en_flat, ce[6]);
	// assert(en_flat.size() == es.size()*vs.size());
	assert(en_flat.size() == es.size() * 2);
	vec<vec<int> > en;
	for (int i = 0; i < es.size(); i++) {
		en.push(vec<int>());
		// The -1 is becaise indexes in MZ start at 1
		en[i].push(en_flat[i] - 1);
		en[i].push(en_flat[es.size() + i] - 1);
	}
	vec<int> ws;
	arg2intargs(ws, ce[7]);
	IntVar* w = getIntVar(ce[8]);

	vec<int> ds;
	for (int i = 0; i < vs.size(); i++) {
		ds.push(0);
	}

	vec<vec<int> > ws2;
	for (int i = 0; i < ws.size(); i++) {
		ws2.push(vec<int>());
		for (int j = 0; j < 100; j++) {
			ws2[ws2.size() - 1].push(ws[i]);
		}
	}

	// vec<IntVar*> lowers;
	// vec<IntVar*> uppers;
	// vec<BoolView> b4_flat;
	// vec<IntVar*> pos;
	// createVars(lowers,vs.size(),0,w->getMax()+1);
	// createVars(uppers,vs.size(),-1,w->getMax());

	bounded_path(from, to, vs, es, in, ou, en, ws, w);
	// td_bounded_path(from, to, vs, es, b4_flat, b4_flat, pos, in, ou, en, ws2, ds, w);//, lowers,
	// uppers);
}

void p_bounded_path_new(const ConExpr& ce, AST::Node* /*ann*/) {
	const int nb_nodes = ce[0]->getInt();
	const int nb_edges = ce[1]->getInt();
	vec<int> from;
	arg2intargs(from, ce[2]);
	vec<int> to;
	arg2intargs(to, ce[3]);
	vec<int> ws;
	arg2intargs(ws, ce[4]);
	const int s = ce[5]->getInt() - 1;
	const int t = ce[6]->getInt() - 1;
	vec<BoolView> vs;
	arg2BoolVarArgs(vs, ce[7]);
	vec<BoolView> es;
	arg2BoolVarArgs(es, ce[8]);
	IntVar* w = getIntVar(ce[9]);

	vec<vec<int> > en;
	vec<vec<int> > in;
	vec<vec<int> > ou;
	for (int i = 0; i < nb_nodes; i++) {
		in.push(vec<int>());
		ou.push(vec<int>());
	}
	for (int i = 0; i < nb_edges; i++) {
		en.push(vec<int>());
		// The -1 is because indexes in MZ start at 1
		const int u = from[i] - 1;
		const int v = to[i] - 1;
		en[i].push(u);
		en[i].push(v);
		ou[u].push(i);
		in[v].push(i);
	}
	path(s, t, vs, es, in, ou, en);
	bounded_path(s, t, vs, es, in, ou, en, ws, w);
}

class IntPoster {
public:
	IntPoster() {
		registry().add("int_eq", &p_int_eq);
		registry().add("int_ne", &p_int_ne);
		registry().add("int_ge", &p_int_ge);
		registry().add("int_gt", &p_int_gt);
		registry().add("int_le", &p_int_le);
		registry().add("int_lt", &p_int_lt);
		registry().add("int_eq_imp", &p_int_eq_imp);
		registry().add("int_ne_imp", &p_int_ne_imp);
		registry().add("int_ge_imp", &p_int_ge_imp);
		registry().add("int_gt_imp", &p_int_gt_imp);
		registry().add("int_le_imp", &p_int_le_imp);
		registry().add("int_lt_imp", &p_int_lt_imp);
		registry().add("int_eq_reif", &p_int_eq_reif);
		registry().add("int_ne_reif", &p_int_ne_reif);
		registry().add("int_ge_reif", &p_int_ge_reif);
		registry().add("int_gt_reif", &p_int_gt_reif);
		registry().add("int_le_reif", &p_int_le_reif);
		registry().add("int_lt_reif", &p_int_lt_reif);
		registry().add("int_lin_eq", &p_int_lin_eq);
		registry().add("int_lin_eq_reif", &p_int_lin_eq_reif);
		registry().add("int_lin_eq_imp", &p_int_lin_eq_imp);
		registry().add("int_lin_ne", &p_int_lin_ne);
		registry().add("int_lin_ne_reif", &p_int_lin_ne_reif);
		registry().add("int_lin_ne_imp", &p_int_lin_ne_imp);
		registry().add("int_lin_le", &p_int_lin_le);
		registry().add("int_lin_le_reif", &p_int_lin_le_reif);
		registry().add("int_lin_le_imp", &p_int_lin_le_imp);
		registry().add("int_lin_lt", &p_int_lin_lt);
		registry().add("int_lin_lt_reif", &p_int_lin_lt_reif);
		registry().add("int_lin_lt_imp", &p_int_lin_lt_imp);
		registry().add("int_lin_ge", &p_int_lin_ge);
		registry().add("int_lin_ge_reif", &p_int_lin_ge_reif);
		registry().add("int_lin_ge_imp", &p_int_lin_ge_imp);
		registry().add("int_lin_gt", &p_int_lin_gt);
		registry().add("int_lin_gt_reif", &p_int_lin_gt_reif);
		registry().add("int_lin_gt_imp", &p_int_lin_gt_imp);
		registry().add("int_plus", &p_int_plus);
		registry().add("int_minus", &p_int_minus);
		registry().add("int_pow", &p_int_pow);
		registry().add("int_times", &p_int_times);
		registry().add("int_div", &p_int_div);
		registry().add("int_mod", &p_int_mod);
		registry().add("int_min", &p_int_min);
		registry().add("int_max", &p_int_max);
		registry().add("int_abs", &p_abs);
		registry().add("int_negate", &p_int_negate);
		registry().add("range_size_fzn", &p_range_size_fzn);
		registry().add("bool_and", &p_bool_and);
		registry().add("bool_not", &p_bool_not);
		registry().add("bool_or", &p_bool_or);
		registry().add("bool_xor", &p_bool_xor);
		registry().add("bool_eq", &p_bool_eq);
		registry().add("bool_eq_imp", &p_bool_eq_imp);
		registry().add("bool_eq_reif", &p_bool_eq_reif);
		registry().add("bool_ne", &p_bool_ne);
		registry().add("bool_ne_reif", &p_bool_ne_reif);
		registry().add("bool_le", &p_bool_le);
		registry().add("bool_le_reif", &p_bool_le_reif);
		registry().add("bool_lt", &p_bool_lt);
		registry().add("bool_lt_reif", &p_bool_lt_reif);
		registry().add("bool_ge", &p_bool_ge);
		registry().add("bool_ge_reif", &p_bool_ge_reif);
		registry().add("bool_gt", &p_bool_gt);
		registry().add("bool_gt_reif", &p_bool_gt_reif);
		registry().add("bool_left_imp", &p_bool_l_imp);
		registry().add("bool_right_imp", &p_bool_r_imp);
		registry().add("array_bool_and", &p_array_bool_and);
		registry().add("array_bool_or", &p_array_bool_or);
		registry().add("bool_clause", &p_array_bool_clause);
		registry().add("bool_clause_reif", &p_array_bool_clause_reif);
		registry().add("array_int_element", &p_array_int_element);
		registry().add("array_var_int_element", &p_array_var_int_element);
		registry().add("array_var_int_element_imp", &p_array_var_int_element_imp);
		registry().add("array_bool_element", &p_array_bool_element);
		registry().add("array_var_bool_element", &p_array_var_bool_element);
		registry().add("bool2int", &p_bool2int);

		registry().add("set_in", &p_set_in);
		registry().add("set_in_reif", &p_set_in_reif);

		registry().add("fzn_all_different_int", &p_all_different_int);
		registry().add("fzn_all_different_int_imp", &p_all_different_int_imp);
		registry().add("inverse_offsets", &p_inverse_offsets);
		registry().add("chuffed_table_int", &p_table_int);
		registry().add("chuffed_regular", &p_regular);
		registry().add("chuffed_cost_regular", &p_cost_regular);
		registry().add("chuffed_disjunctive_strict", &p_disjunctive);
		registry().add("chuffed_cumulative", &p_cumulative);
		registry().add("chuffed_cumulative_vars", &p_cumulative2);
		registry().add("chuffed_cumulative_cal", &p_cumulative_cal);
		registry().add("chuffed_circuit", &p_circuit);
		registry().add("chuffed_subcircuit", &p_subcircuit);
		registry().add("chuffed_array_int_minimum", &p_minimum);
		registry().add("chuffed_array_int_maximum", &p_maximum);
		registry().add("chuffed_maximum_arg_bool", &p_bool_arg_max);
		registry().add("lex_less_int", &p_lex_less);
		registry().add("lex_lesseq_int", &p_lex_lesseq);
		registry().add("chuffed_edit_distance", &p_edit_distance);

		registry().add("variables_interchange", &var_sym);
		registry().add("values_interchange", &val_sym);
		registry().add("variables_sequences", &var_seq_sym);
		registry().add("values_sequences", &val_seq_sym);
		registry().add("chuffed_value_precede", &val_prec);
		registry().add("chuffed_seq_precede", &val_prec_seq);

		/*
						registry().add("all_different_int", &p_distinct);
						registry().add("all_different_offset", &p_distinctOffset);
						registry().add("count", &p_count);
						registry().add("at_least_int", &p_at_least);
						registry().add("at_most_int", &p_at_most);
						registry().add("global_cardinality_gecode", &p_global_cardinality);
						registry().add("sort", &p_sort);
						registry().add("inverse_offsets", &p_inverse_offsets);
						registry().add("increasing_int", &p_increasing_int);
						registry().add("increasing_bool", &p_increasing_bool);
						registry().add("table_bool", &p_table_bool);
						registry().add("cumulatives", &p_cumulatives);
		*/
		registry().add("bool_sum_eq", &p_bool_sum_eq);
		registry().add("bool_sum_le", &p_bool_sum_le);
		registry().add("bool_sum_lt", &p_bool_sum_lt);
		registry().add("bool_sum_ge", &p_bool_sum_ge);
		registry().add("bool_sum_gt", &p_bool_sum_gt);
		/*
						registry().add("bool_lin_eq", &p_bool_lin_eq);
						registry().add("bool_lin_ne", &p_bool_lin_ne);
						registry().add("bool_lin_le", &p_bool_lin_le);
						registry().add("bool_lin_lt", &p_bool_lin_lt);
						registry().add("bool_lin_ge", &p_bool_lin_ge);
						registry().add("bool_lin_gt", &p_bool_lin_gt);

						registry().add("bool_lin_eq_reif", &p_bool_lin_eq_reif);
						registry().add("bool_lin_ne_reif", &p_bool_lin_ne_reif);
						registry().add("bool_lin_le_reif", &p_bool_lin_le_reif);
						registry().add("bool_lin_lt_reif", &p_bool_lin_lt_reif);
						registry().add("bool_lin_ge_reif", &p_bool_lin_ge_reif);
						registry().add("bool_lin_gt_reif", &p_bool_lin_gt_reif);
		*/

		registry().add("chuffed_tree", &p_tree_new);
		registry().add("chuffed_connected", &p_connected_new);
		registry().add("chuffed_steiner", &p_steiner_tree_new);
		registry().add("chuffed_minimal_spanning_tree", &p_mst_new);
		registry().add("chuffed_dtree", &p_dtree_new);
		registry().add("chuffed_dpath", &p_path_new);
		registry().add("chuffed_dag", &p_dag_new);
		registry().add("chuffed_bounded_dpath", &p_bounded_path_new);
	}
};
IntPoster _int_poster;

}  // namespace

}  // namespace FlatZinc
