#include "extract.h"

#include <queue>
#include <vector>
#include <utility>
#include <tr1/unordered_map>
#include <set>

#include <boost/functional/hash.hpp>

#include "sentence_pair.h"
#include "tdict.h"
#include "wordid.h"
#include "array2d.h"

using namespace std;
using namespace std::tr1;

void Extract::ExtractBasePhrases(const int max_base_phrase_size,
                        const AnnotatedParallelSentence& sentence,
                        vector<ParallelSpan>* phrases) {
  phrases->clear();

  vector<pair<int,int> > f_spans(sentence.f_len, pair<int,int>(sentence.e_len, 0));
  vector<pair<int,int> > e_spans(sentence.e_len, pair<int,int>(sentence.f_len, 0));
  // for each alignment point in e, precompute the minimal consistent phrases in f
  // for each alignment point in f, precompute the minimal consistent phrases in e
  for (int i = 0; i < sentence.f_len; ++i) {
    for (int j = 0; j < sentence.e_len; ++j) {
      if (sentence.aligned(i,j)) {
        if (j < f_spans[i].first) f_spans[i].first = j;
        f_spans[i].second = j+1;
        if (i < e_spans[j].first) e_spans[j].first = i;
        e_spans[j].second = i+1;
      }
    }
  }

  for (int i1 = 0; i1 < sentence.f_len; ++i1) {
    if (sentence.f_aligned[i1] == 0) continue;
    int j1 = sentence.e_len;
    int j2 = 0;
    const int i_limit = min(sentence.f_len, i1 + max_base_phrase_size);
    for (int i2 = i1 + 1; i2 <= i_limit; ++i2) {
      if (sentence.f_aligned[i2-1] == 0) continue;
      // cerr << "F has aligned span " << i1 << " to " << i2 << endl;
      j1 = min(j1, f_spans[i2-1].first);
      j2 = max(j2, f_spans[i2-1].second);
      if (j1 >= j2) continue;
      if (j2 - j1 > max_base_phrase_size) continue;
      int condition = 0;
      for (int j = j1; j < j2; ++j) {
        if (e_spans[j].first < i1) { condition = 1; break; }
        if (e_spans[j].second > i2) { condition = 2; break; }
      }
      if (condition == 1) break;
      if (condition == 2) continue;
      // category types added later!
      phrases->push_back(ParallelSpan(i1, i2, j1, j2));
      // cerr << i1 << " " << i2 << " : " << j1 << " " << j2 << endl;
    }
  }
}

// TODO implement phrase loosening!
void Extract::LoosenPhraseBounds(const AnnotatedParallelSentence& sentence,
                        vector<ParallelSpan>* phrases) {
  assert(!"not implemented");
  (void) sentence;
  (void) phrases;
}

// this uses the TARGET span (i,j) to annotate phrases, will copy
// phrases if there is more than one annotation.
// TODO: support source annotation
void Extract::AnnotatePhrasesWithCategoryTypes(const WordID default_cat,
                                      const Array2D<vector<WordID> >& types,
                                      vector<ParallelSpan>* phrases) {
  const int num_unannotated_phrases = phrases->size();
  // have to use num_unannotated_phrases since we may grow the vector
  for (int i = 0; i < num_unannotated_phrases; ++i) {
    ParallelSpan& phrase = (*phrases)[i];
    const vector<WordID>* pcats = &types(phrase.j1, phrase.j2);
    if (pcats->empty() && default_cat != 0) {
      static vector<WordID> s_default(1, default_cat);
      pcats = &s_default;
    }
    if (pcats->empty()) {
      cerr << "ERROR span " << phrase.i1 << "," << phrase.i2 << "-"
           << phrase.j1 << "," << phrase.j2 << " has no type. "
              "Did you forget --default_category?\n";
    }
    const vector<WordID>& cats = *pcats;
    phrase.cat = cats[0];
    for (int ci = 1; ci < cats.size(); ++ci) {
      ParallelSpan new_phrase = phrase;
      new_phrase.cat = cats[ci];
      phrases->push_back(new_phrase);
    }
  }
}

// a partially complete (f-side) of a rule
struct RuleItem {
  vector<ParallelSpan> f;
  int i,j,syms,vars;
  explicit RuleItem(int pi) : i(pi), j(pi), syms(), vars() {}
  void Extend(const WordID& fword) {
    f.push_back(ParallelSpan(fword));
    ++j;
    ++syms;
  }
  void Extend(const ParallelSpan& subphrase) {
    f.push_back(subphrase);
    j += subphrase.i2 - subphrase.i1;
    ++vars;
    ++syms;
  }
  bool RuleFEndsInVariable() const {
    if (f.size() > 0) {
      return f.back().IsVariable();
    } else { return false; }
  }
};

void Extract::ExtractConsistentRules(const AnnotatedParallelSentence& sentence,
                          const vector<ParallelSpan>& phrases,
                          const int max_vars,
                          const int max_syms,
                          const bool permit_adjacent_nonterminals,
                          RuleObserver* observer) {
  queue<RuleItem> q;  // agenda for BFS
  int max_len = -1;
  unordered_map<pair<short, short>, vector<ParallelSpan>, boost::hash<pair<short, short> > > fspans;
  vector<vector<ParallelSpan> > spans_by_start(sentence.f_len);
  set<int> starts;
  for (int i = 0; i < phrases.size(); ++i) {
    fspans[make_pair(phrases[i].i1,phrases[i].i2)].push_back(phrases[i]);
    max_len = max(max_len, phrases[i].i2 - phrases[i].i1);
    // have we already added a rule item starting at phrases[i].i1?
    if (starts.insert(phrases[i].i1).second)
      q.push(RuleItem(phrases[i].i1));
    spans_by_start[phrases[i].i1].push_back(phrases[i]);
  }
  starts.clear();
  vector<pair<int,int> > next_e(sentence.e_len);
  vector<WordID> cur_rhs_f, cur_rhs_e;
  vector<pair<int, int> > cur_terminal_align;
  while(!q.empty()) {
    const RuleItem& rule = q.front();

    // extend the partial rule
    if (rule.j < sentence.f_len && (rule.j - rule.i) < max_len && rule.syms < max_syms) {
      RuleItem ew = rule;

      // extend with a word
      ew.Extend(sentence.f[ew.j]);
      q.push(ew);

      // with variables
      if (rule.vars < max_vars &&
          !spans_by_start[rule.j].empty() &&
          ((!rule.RuleFEndsInVariable()) || permit_adjacent_nonterminals)) {
        const vector<ParallelSpan>& sub_phrases = spans_by_start[rule.j];
        for (int it = 0; it < sub_phrases.size(); ++it) {
          if (sub_phrases[it].i2 - sub_phrases[it].i1 + rule.j - rule.i <= max_len) {
            RuleItem ev = rule;
            ev.Extend(sub_phrases[it]);
            q.push(ev);
            assert(ev.j <= sentence.f_len);
          }
        }
      }
    }
    // determine if rule is consistent
    if (rule.syms > 0 &&
        fspans.count(make_pair(rule.i,rule.j)) &&
        (!rule.RuleFEndsInVariable() || rule.syms > 1)) {
      const vector<ParallelSpan>& orig_spans = fspans[make_pair(rule.i,rule.j)];
      for (int s = 0; s < orig_spans.size(); ++s) {
        const ParallelSpan& orig_span = orig_spans[s];
        const WordID lhs = orig_span.cat;
        for (int j = orig_span.j1; j < orig_span.j2; ++j) next_e[j].first = -1;
        int nt_index_e = 0;
        for (int i = 0; i < rule.f.size(); ++i) {
          const ParallelSpan& cur = rule.f[i];
          if (cur.IsVariable())
            next_e[cur.j1] = pair<int,int>(cur.j2, ++nt_index_e);
        }
        cur_rhs_f.clear();
        cur_rhs_e.clear();
        cur_terminal_align.clear();

        for (int i = 0; i < rule.f.size(); ++i) {
          const ParallelSpan& cur = rule.f[i];
          cur_rhs_f.push_back(cur.cat);
        }
        for (int j = orig_span.j1; j < orig_span.j2; ++j) {
          if (next_e[j].first < 0) {
            cur_rhs_e.push_back(sentence.e[j]);
          } else {
            cur_rhs_e.push_back(1 - next_e[j].second);  // next_e[j].second is NT gap index
            j = next_e[j].first - 1;
          }
        }
        // TODO set cur_terminal_align
        observer->CountRule(lhs, cur_rhs_f, cur_rhs_e, cur_terminal_align);
      }
    }
    q.pop();
  }
}

