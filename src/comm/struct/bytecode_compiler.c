#include "bytecode.h"
#include "compiler.h"
#include "crypto.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
  char strings[256][128];
  uint32_t count;
} ConstPoolBuilder;

typedef struct {
  JobNode *job;
  uint32_t base_size;
  uint32_t start_offset;
  uint32_t total_size;
  bool append_return;
  bool append_jump_end;
  uint32_t jump_target_offset;
} JobInfo;

static uint32_t add_constant(ConstPoolBuilder *cp, const char *str, size_t len) {
  /*#region*/
  for (uint32_t i = 0; i < cp->count; i++) {
    if (strncmp(cp->strings[i], str, len) == 0 && cp->strings[i][len] == '\0') {
      return i;
    }
  }
  if (cp->count >= 256) return 0;
  uint32_t idx = cp->count++;
  size_t copy_len = len < 127 ? len : 127;
  memcpy(cp->strings[idx], str, copy_len);
  cp->strings[idx][copy_len] = '\0';
  return idx;
  /*#endregion*/
}

static int get_job_index(WorkflowAST *ast, JobNode *job) {
  /*#region*/
  int idx = 0;
  JobNode *curr = ast->jobs_head;
  while (curr) {
    if (curr == job) return idx;
    curr = curr->next_sorted;
    idx++;
  }
  return -1;
  /*#endregion*/
}

static JobNode *find_job_by_id(WorkflowAST *ast, StringView id) {
  /*#region*/
  JobNode *curr = ast->jobs_head;
  while (curr) {
    if (sv_compare(curr->id, id) == 0) return curr;
    curr = curr->next_sorted;
  }
  return NULL;
  /*#endregion*/
}

static void mark_fork_descendants(WorkflowAST *ast, JobNode *node, bool *in_fork) {
  /*#region*/
  // Recursively traverses the DAG forward to mark all nodes spawned as part of a fork.
  int idx = get_job_index(ast, node);
  if (idx < 0 || in_fork[idx] || node->type == NODE_JOIN) return;
  in_fork[idx] = true;
  
  JobNode *curr = ast->jobs_head;
  while (curr) {
    for (size_t d = 0; d < curr->dependency_count; d++) {
      if (curr->depends_on_nodes[d] == node) {
        mark_fork_descendants(ast, curr, in_fork);
      }
    }
    curr = curr->next_sorted;
  }
  /*#endregion*/
}

static JobNode *find_job_by_id_array(JobNode **nodes, int count, StringView id) {
  /*#region*/
  for (int i = 0; i < count; i++) {
    if (sv_compare(nodes[i]->id, id) == 0) return nodes[i];
  }
  return NULL;
  /*#endregion*/
}

static bool is_branch_job_array(JobNode **nodes, int count, JobNode *job) {
  /*#region*/
  for (int i = 0; i < count; i++) {
    JobNode *curr = nodes[i];
    if (curr->type == NODE_IF) {
      for (size_t b = 0; b < curr->spec.binary_if.then_count; b++) {
        if (sv_compare(curr->spec.binary_if.then_branch[b], job->id) == 0) return true;
      }
      for (size_t b = 0; b < curr->spec.binary_if.else_count; b++) {
        if (sv_compare(curr->spec.binary_if.else_branch[b], job->id) == 0) return true;
      }
    } else if (curr->type == NODE_SWITCH) {
      SwitchCase *sc = curr->spec.multi_switch.cases;
      while (sc) {
        for (size_t b = 0; b < sc->then_count; b++) {
          if (sv_compare(sc->then_branch[b], job->id) == 0) return true;
        }
        sc = sc->next;
      }
      for (size_t b = 0; b < curr->spec.multi_switch.default_count; b++) {
        if (sv_compare(curr->spec.multi_switch.default_branch[b], job->id) == 0) return true;
      }
    } else if (curr->type == NODE_FORK) {
      for (size_t b = 0; b < curr->spec.fork_node.branch_count; b++) {
        if (sv_compare(curr->spec.fork_node.branches[b], job->id) == 0) return true;
      }
    }
  }
  return false;
  /*#endregion*/
}

static void visit_job(WorkflowAST *ast, JobNode **nodes, int count, JobNode *job, JobNode **sorted_arr, int *sorted_count, bool *visited) {
  /*#region*/
  int idx = get_job_index(ast, job);
  if (idx < 0 || visited[idx]) return;

  for (size_t d = 0; d < job->dependency_count; d++) {
    JobNode *dep = job->depends_on_nodes[d];
    if (dep) {
      int dep_idx = get_job_index(ast, dep);
      if (dep_idx >= 0 && !visited[dep_idx]) {
        return;
      }
    }
  }

  visited[idx] = true;
  sorted_arr[(*sorted_count)++] = job;

  if (job->type == NODE_IF) {
    for (size_t b = 0; b < job->spec.binary_if.then_count; b++) {
      JobNode *b_job = find_job_by_id_array(nodes, count, job->spec.binary_if.then_branch[b]);
      if (b_job) visit_job(ast, nodes, count, b_job, sorted_arr, sorted_count, visited);
    }
    for (size_t b = 0; b < job->spec.binary_if.else_count; b++) {
      JobNode *b_job = find_job_by_id_array(nodes, count, job->spec.binary_if.else_branch[b]);
      if (b_job) visit_job(ast, nodes, count, b_job, sorted_arr, sorted_count, visited);
    }
  } else if (job->type == NODE_SWITCH) {
    SwitchCase *sc = job->spec.multi_switch.cases;
    while (sc) {
      for (size_t b = 0; b < sc->then_count; b++) {
        JobNode *b_job = find_job_by_id_array(nodes, count, sc->then_branch[b]);
        if (b_job) visit_job(ast, nodes, count, b_job, sorted_arr, sorted_count, visited);
      }
      sc = sc->next;
    }
    for (size_t b = 0; b < job->spec.multi_switch.default_count; b++) {
      JobNode *b_job = find_job_by_id_array(nodes, count, job->spec.multi_switch.default_branch[b]);
      if (b_job) visit_job(ast, nodes, count, b_job, sorted_arr, sorted_count, visited);
    }
  } else if (job->type == NODE_FORK) {
    for (size_t b = 0; b < job->spec.fork_node.branch_count; b++) {
      JobNode *b_job = find_job_by_id_array(nodes, count, job->spec.fork_node.branches[b]);
      if (b_job) visit_job(ast, nodes, count, b_job, sorted_arr, sorted_count, visited);
    }
  }
  /*#endregion*/
}

int32_t bytecode_compile_workflow(Arena *arena, WorkflowAST *ast, const char *output_nbc_path) {
  /*#region*/
  if (!ast || !output_nbc_path) return ERR_INVALID_BOUNDARY;

  // Count jobs first to allocate arrays
  int orig_job_count = 0;
  JobNode *temp = ast->jobs_head;
  while (temp) {
    orig_job_count++;
    temp = temp->next_sorted;
  }

  if (orig_job_count > 0) {
    JobNode **orig_nodes = na_alloc(arena, orig_job_count * sizeof(JobNode *));
    JobNode **sorted_arr = na_alloc(arena, orig_job_count * sizeof(JobNode *));
    bool *visited = na_alloc(arena, orig_job_count * sizeof(bool));
    if (!orig_nodes || !sorted_arr || !visited) return ERR_OOM;

    memset(visited, 0, orig_job_count * sizeof(bool));

    temp = ast->jobs_head;
    int count = 0;
    while (temp) {
      orig_nodes[count++] = temp;
      temp = temp->next_sorted;
    }

    int sorted_count = 0;

    // 1. Visit root non-branch jobs
    for (int i = 0; i < orig_job_count; i++) {
      JobNode *job = orig_nodes[i];
      if (!is_branch_job_array(orig_nodes, orig_job_count, job)) {
        bool has_deps = false;
        for (size_t d = 0; d < job->dependency_count; d++) {
          if (job->depends_on_nodes[d]) {
            has_deps = true;
            break;
          }
        }
        if (!has_deps) {
          visit_job(ast, orig_nodes, orig_job_count, job, sorted_arr, &sorted_count, visited);
        }
      }
    }

    // 2. Visit any remaining jobs
    for (int i = 0; i < orig_job_count; i++) {
      visit_job(ast, orig_nodes, orig_job_count, orig_nodes[i], sorted_arr, &sorted_count, visited);
    }

    // Reconstruct the next_sorted list from sorted_arr
    for (int i = 0; i < sorted_count - 1; i++) {
      sorted_arr[i]->next_sorted = sorted_arr[i + 1];
    }
    if (sorted_count > 0) {
      sorted_arr[sorted_count - 1]->next_sorted = NULL;
      ast->jobs_head = sorted_arr[0];
    }
  }

  ConstPoolBuilder cp = {0};
  add_constant(&cp, "NEST_INIT", 9);

  int job_count = 0;
  temp = ast->jobs_head;
  while (temp) {
    job_count++;
    temp = temp->next_sorted;
  }

  JobInfo *infos = NULL;
  bool *in_fork = NULL;
  if (job_count > 0) {
    infos = na_alloc(arena, job_count * sizeof(JobInfo));
    in_fork = na_alloc(arena, job_count * sizeof(bool));
    if (!infos || !in_fork) return ERR_OOM;
    memset(infos, 0, job_count * sizeof(JobInfo));
    memset(in_fork, 0, job_count * sizeof(bool));
  }

  // Pass 0: Add constants and compute base sizes of individual jobs.
  temp = ast->jobs_head;
  int idx = 0;
  while (temp) {
    infos[idx].job = temp;
    add_constant(&cp, temp->id.data ? temp->id.data : "", temp->id.length);
    
    if (temp->type == NODE_IF) {
      add_constant(&cp, temp->spec.binary_if.condition.data ? temp->spec.binary_if.condition.data : "", temp->spec.binary_if.condition.length);
      uint32_t size = 15; // OP_COMPLETE_JOB + OP_RESOLVE + OP_JUMP_IF_FALSE
      for (size_t b = 0; b < temp->spec.binary_if.else_count; b++) {
        JobNode *b_job = find_job_by_id(ast, temp->spec.binary_if.else_branch[b]);
        if (b_job) {
          add_constant(&cp, b_job->id.data, b_job->id.length);
        }
      }
      if (temp->spec.binary_if.else_count > 0) {
        size += 5 * temp->spec.binary_if.else_count;
        if (temp->spec.binary_if.then_count > 0) {
          size += 5;
        }
      }
      if (temp->spec.binary_if.then_count > 0 && temp->spec.binary_if.else_count > 0) {
        for (size_t b = 0; b < temp->spec.binary_if.then_count; b++) {
          JobNode *b_job = find_job_by_id(ast, temp->spec.binary_if.then_branch[b]);
          if (b_job) {
            add_constant(&cp, b_job->id.data, b_job->id.length);
          }
        }
        size += 5 * temp->spec.binary_if.then_count + 5;
      }
      infos[idx].base_size = size;
    } else if (temp->type == NODE_SWITCH) {
      uint32_t sc_size = 5; // OP_COMPLETE_JOB
      uint32_t total_cases_jobs = 0;
      SwitchCase *sc = temp->spec.multi_switch.cases;
      while (sc) {
        total_cases_jobs += sc->then_count;
        sc = sc->next;
      }
      uint32_t total_default_jobs = temp->spec.multi_switch.default_count;

      sc = temp->spec.multi_switch.cases;
      while (sc) {
        add_constant(&cp, sc->condition.data ? sc->condition.data : "", sc->condition.length);
        sc_size += 10; // OP_RESOLVE + OP_JUMP_IF_FALSE
        uint32_t other_jobs = (total_cases_jobs - sc->then_count) + total_default_jobs;
        sc_size += 5 * other_jobs; // OP_SKIP_JOB
        sc_size += 5; // OP_JUMP
        
        // Add other cases' job constants
        SwitchCase *other_sc = temp->spec.multi_switch.cases;
        while (other_sc) {
          if (other_sc != sc) {
            for (size_t b = 0; b < other_sc->then_count; b++) {
              JobNode *b_job = find_job_by_id(ast, other_sc->then_branch[b]);
              if (b_job) add_constant(&cp, b_job->id.data, b_job->id.length);
            }
          }
          other_sc = other_sc->next;
        }
        for (size_t b = 0; b < temp->spec.multi_switch.default_count; b++) {
          JobNode *b_job = find_job_by_id(ast, temp->spec.multi_switch.default_branch[b]);
          if (b_job) add_constant(&cp, b_job->id.data, b_job->id.length);
        }
        sc = sc->next;
      }
      
      if (total_default_jobs > 0) {
        sc_size += 5 * total_cases_jobs;
      } else {
        sc_size += 5 * total_cases_jobs + 5;
      }
      
      // Add all cases' job constants for default block
      sc = temp->spec.multi_switch.cases;
      while (sc) {
        for (size_t b = 0; b < sc->then_count; b++) {
          JobNode *b_job = find_job_by_id(ast, sc->then_branch[b]);
          if (b_job) add_constant(&cp, b_job->id.data, b_job->id.length);
        }
        sc = sc->next;
      }
      
      infos[idx].base_size = sc_size;
    } else if (temp->type == NODE_FORK) {
      infos[idx].base_size = 5 + 7 + 4 * temp->spec.fork_node.branch_count; // OP_COMPLETE_JOB + OP_FORK count + targets[] + OP_JUMP past
    } else if (temp->type == NODE_JOIN) {
      infos[idx].base_size = 6; // OP_JOIN strategy + join_job_idx
    } else {
      infos[idx].base_size = 5; // OP_CALL_PROVIDER
    }
    
    temp = temp->next_sorted;
    idx++;
  }

  // Pass 1: Mark fork branch descendants and identify leaf nodes needing OP_RETURN.
  temp = ast->jobs_head;
  idx = 0;
  while (temp) {
    if (temp->type == NODE_FORK) {
      for (size_t b = 0; b < temp->spec.fork_node.branch_count; b++) {
        JobNode *branch_job = find_job_by_id(ast, temp->spec.fork_node.branches[b]);
        if (branch_job) {
          mark_fork_descendants(ast, branch_job, in_fork);
        }
      }
    }
    temp = temp->next_sorted;
    idx++;
  }

  for (int i = 0; i < job_count; i++) {
    if (in_fork[i]) {
      bool is_leaf = true;
      JobNode *job = infos[i].job;
      JobNode *curr = ast->jobs_head;
      while (curr) {
        for (size_t d = 0; d < curr->dependency_count; d++) {
          if (curr->depends_on_nodes[d] == job && curr->type != NODE_JOIN) {
            is_leaf = false;
            break;
          }
        }
        if (!is_leaf) break;
        curr = curr->next_sorted;
      }
      if (is_leaf) {
        infos[i].append_return = true;
      }
    }
  }

  // Pass 2: Handle NODE_IF and NODE_SWITCH branch end jumps.
  temp = ast->jobs_head;
  idx = 0;
  while (temp) {
    if (temp->type == NODE_IF) {
      if (temp->spec.binary_if.else_count > 0) {
        int last_then_idx = -1;
        for (size_t b = 0; b < temp->spec.binary_if.then_count; b++) {
          JobNode *b_job = find_job_by_id(ast, temp->spec.binary_if.then_branch[b]);
          if (b_job) {
            int b_idx = get_job_index(ast, b_job);
            if (b_idx > last_then_idx) last_then_idx = b_idx;
          }
        }
        if (last_then_idx >= 0) {
          infos[last_then_idx].append_jump_end = true;
        }
      }
    } else if (temp->type == NODE_SWITCH) {
      SwitchCase *sc = temp->spec.multi_switch.cases;
      while (sc) {
        int last_case_idx = -1;
        for (size_t b = 0; b < sc->then_count; b++) {
          JobNode *b_job = find_job_by_id(ast, sc->then_branch[b]);
          if (b_job) {
            int b_idx = get_job_index(ast, b_job);
            if (b_idx > last_case_idx) last_case_idx = b_idx;
          }
        }
        if (last_case_idx >= 0) {
          infos[last_case_idx].append_jump_end = true;
        }
        sc = sc->next;
      }
    }
    temp = temp->next_sorted;
    idx++;
  }

  for (int i = 0; i < job_count; i++) {
    if (in_fork[i] && !infos[i].append_return) {
      infos[i].append_jump_end = true;
    }
  }

  // Pass 3: Compute starting byte offsets for all jobs.
  uint32_t current_offset = 0;
  for (int i = 0; i < job_count; i++) {
    infos[i].start_offset = current_offset;
    uint32_t size = infos[i].base_size;
    if (infos[i].append_return) size += 1;
    if (infos[i].append_jump_end) size += 5;
    infos[i].total_size = size;
    current_offset += size;
  }
  uint32_t global_return_offset = current_offset;
  (void)global_return_offset;
  current_offset += 1;
  uint32_t total_code_size = current_offset;

  // Pass 4: Resolve jump targets.
  for (int i = 0; i < job_count; i++) {
    JobNode *job = infos[i].job;
    if (job->type == NODE_IF) {
      uint32_t end_if_offset = infos[i].start_offset + infos[i].total_size;
      for (size_t b = 0; b < job->spec.binary_if.then_count; b++) {
        JobNode *b_job = find_job_by_id(ast, job->spec.binary_if.then_branch[b]);
        if (b_job) {
          int b_idx = get_job_index(ast, b_job);
          uint32_t end_offset = infos[b_idx].start_offset + infos[b_idx].total_size;
          if (end_offset > end_if_offset) end_if_offset = end_offset;
        }
      }
      for (size_t b = 0; b < job->spec.binary_if.else_count; b++) {
        JobNode *b_job = find_job_by_id(ast, job->spec.binary_if.else_branch[b]);
        if (b_job) {
          int b_idx = get_job_index(ast, b_job);
          uint32_t end_offset = infos[b_idx].start_offset + infos[b_idx].total_size;
          if (end_offset > end_if_offset) end_if_offset = end_offset;
        }
      }
      
      for (size_t b = 0; b < job->spec.binary_if.then_count; b++) {
        JobNode *b_job = find_job_by_id(ast, job->spec.binary_if.then_branch[b]);
        if (b_job) {
          int b_idx = get_job_index(ast, b_job);
          if (infos[b_idx].append_jump_end) {
            infos[b_idx].jump_target_offset = end_if_offset;
          }
        }
      }
      
      uint32_t else_target_offset = end_if_offset;
      if (job->spec.binary_if.else_count > 0) {
        JobNode *first_else = find_job_by_id(ast, job->spec.binary_if.else_branch[0]);
        if (first_else) {
          int fe_idx = get_job_index(ast, first_else);
          else_target_offset = infos[fe_idx].start_offset;
        }
      }
      infos[i].jump_target_offset = else_target_offset;
      
    } else if (job->type == NODE_SWITCH) {
      uint32_t end_switch_offset = infos[i].start_offset + infos[i].total_size;
      SwitchCase *sc = job->spec.multi_switch.cases;
      while (sc) {
        for (size_t b = 0; b < sc->then_count; b++) {
          JobNode *b_job = find_job_by_id(ast, sc->then_branch[b]);
          if (b_job) {
            int b_idx = get_job_index(ast, b_job);
            uint32_t end_offset = infos[b_idx].start_offset + infos[b_idx].total_size;
            if (end_offset > end_switch_offset) end_switch_offset = end_offset;
          }
        }
        sc = sc->next;
      }
      for (size_t b = 0; b < job->spec.multi_switch.default_count; b++) {
        JobNode *b_job = find_job_by_id(ast, job->spec.multi_switch.default_branch[b]);
        if (b_job) {
          int b_idx = get_job_index(ast, b_job);
          uint32_t end_offset = infos[b_idx].start_offset + infos[b_idx].total_size;
          if (end_offset > end_switch_offset) end_switch_offset = end_offset;
        }
      }
      
      sc = job->spec.multi_switch.cases;
      while (sc) {
        for (size_t b = 0; b < sc->then_count; b++) {
          JobNode *b_job = find_job_by_id(ast, sc->then_branch[b]);
          if (b_job) {
            int b_idx = get_job_index(ast, b_job);
            if (infos[b_idx].append_jump_end) {
              infos[b_idx].jump_target_offset = end_switch_offset;
            }
          }
        }
        sc = sc->next;
      }
      
    } else if (job->type == NODE_FORK) {
      uint32_t end_fork_offset = infos[i].start_offset + infos[i].total_size;
      for (int j = 0; j < job_count; j++) {
        if (in_fork[j]) {
          uint32_t end_offset = infos[j].start_offset + infos[j].total_size;
          if (end_offset > end_fork_offset) end_fork_offset = end_offset;
        }
      }
      infos[i].jump_target_offset = end_fork_offset;
    }

    if (in_fork[i] && infos[i].append_jump_end) {
      uint32_t target_offset = global_return_offset;
      for (int j = i + 1; j < job_count; j++) {
        JobNode *curr = infos[j].job;
        for (size_t d = 0; d < curr->dependency_count; d++) {
          if (curr->depends_on_nodes[d] == job) {
            target_offset = infos[j].start_offset;
            break;
          }
        }
        if (target_offset != global_return_offset) break;
      }
      infos[i].jump_target_offset = target_offset;
    }
  }

  // Pass 5: Output generated instructions.
  uint8_t *code_buf = na_alloc(arena, total_code_size);
  if (!code_buf) return ERR_OOM;
  uint32_t code_size = 0;

  for (int i = 0; i < job_count; i++) {
    JobNode *job = infos[i].job;
    
    if (job->type == NODE_IF) {
      uint32_t job_id_const = add_constant(&cp, job->id.data ? job->id.data : "", job->id.length);
      code_buf[code_size++] = OP_COMPLETE_JOB;
      code_buf[code_size++] = (job_id_const >> 24) & 0xFF;
      code_buf[code_size++] = (job_id_const >> 16) & 0xFF;
      code_buf[code_size++] = (job_id_const >> 8) & 0xFF;
      code_buf[code_size++] = job_id_const & 0xFF;

      uint32_t cond_const = add_constant(&cp, job->spec.binary_if.condition.data ? job->spec.binary_if.condition.data : "", job->spec.binary_if.condition.length);
      code_buf[code_size++] = OP_RESOLVE;
      code_buf[code_size++] = (cond_const >> 24) & 0xFF;
      code_buf[code_size++] = (cond_const >> 16) & 0xFF;
      code_buf[code_size++] = (cond_const >> 8) & 0xFF;
      code_buf[code_size++] = cond_const & 0xFF;
      
      uint32_t else_skip_offset = infos[i].start_offset + 15;
      if (job->spec.binary_if.else_count > 0) {
        else_skip_offset += 5 * job->spec.binary_if.else_count;
        if (job->spec.binary_if.then_count > 0) {
          else_skip_offset += 5;
        }
      } else {
        else_skip_offset = infos[i].start_offset + infos[i].total_size;
      }

      code_buf[code_size++] = OP_JUMP_IF_FALSE;
      code_buf[code_size++] = (else_skip_offset >> 24) & 0xFF;
      code_buf[code_size++] = (else_skip_offset >> 16) & 0xFF;
      code_buf[code_size++] = (else_skip_offset >> 8) & 0xFF;
      code_buf[code_size++] = else_skip_offset & 0xFF;

      if (job->spec.binary_if.else_count > 0) {
        for (size_t b = 0; b < job->spec.binary_if.else_count; b++) {
          JobNode *b_job = find_job_by_id(ast, job->spec.binary_if.else_branch[b]);
          uint32_t b_const = add_constant(&cp, b_job->id.data, b_job->id.length);
          code_buf[code_size++] = OP_SKIP_JOB;
          code_buf[code_size++] = (b_const >> 24) & 0xFF;
          code_buf[code_size++] = (b_const >> 16) & 0xFF;
          code_buf[code_size++] = (b_const >> 8) & 0xFF;
          code_buf[code_size++] = b_const & 0xFF;
        }
        if (job->spec.binary_if.then_count > 0) {
          JobNode *first_then = find_job_by_id(ast, job->spec.binary_if.then_branch[0]);
          int ft_idx = get_job_index(ast, first_then);
          uint32_t ft_offset = infos[ft_idx].start_offset;
          code_buf[code_size++] = OP_JUMP;
          code_buf[code_size++] = (ft_offset >> 24) & 0xFF;
          code_buf[code_size++] = (ft_offset >> 16) & 0xFF;
          code_buf[code_size++] = (ft_offset >> 8) & 0xFF;
          code_buf[code_size++] = ft_offset & 0xFF;
        }
      }

      if (job->spec.binary_if.then_count > 0 && job->spec.binary_if.else_count > 0) {
        for (size_t b = 0; b < job->spec.binary_if.then_count; b++) {
          JobNode *b_job = find_job_by_id(ast, job->spec.binary_if.then_branch[b]);
          uint32_t b_const = add_constant(&cp, b_job->id.data, b_job->id.length);
          code_buf[code_size++] = OP_SKIP_JOB;
          code_buf[code_size++] = (b_const >> 24) & 0xFF;
          code_buf[code_size++] = (b_const >> 16) & 0xFF;
          code_buf[code_size++] = (b_const >> 8) & 0xFF;
          code_buf[code_size++] = b_const & 0xFF;
        }

        JobNode *first_else = find_job_by_id(ast, job->spec.binary_if.else_branch[0]);
        int fe_idx = get_job_index(ast, first_else);
        uint32_t fe_offset = infos[fe_idx].start_offset;
        code_buf[code_size++] = OP_JUMP;
        code_buf[code_size++] = (fe_offset >> 24) & 0xFF;
        code_buf[code_size++] = (fe_offset >> 16) & 0xFF;
        code_buf[code_size++] = (fe_offset >> 8) & 0xFF;
        code_buf[code_size++] = fe_offset & 0xFF;
      }
      
    } else if (job->type == NODE_SWITCH) {
      uint32_t job_id_const = add_constant(&cp, job->id.data ? job->id.data : "", job->id.length);
      code_buf[code_size++] = OP_COMPLETE_JOB;
      code_buf[code_size++] = (job_id_const >> 24) & 0xFF;
      code_buf[code_size++] = (job_id_const >> 16) & 0xFF;
      code_buf[code_size++] = (job_id_const >> 8) & 0xFF;
      code_buf[code_size++] = job_id_const & 0xFF;

      uint32_t total_cases_jobs = 0;
      SwitchCase *sc = job->spec.multi_switch.cases;
      while (sc) {
        total_cases_jobs += sc->then_count;
        sc = sc->next;
      }
      uint32_t total_default_jobs = job->spec.multi_switch.default_count;
      uint32_t next_case_offset = infos[i].start_offset + 5;

      sc = job->spec.multi_switch.cases;
      while (sc) {
        uint32_t cond_const = add_constant(&cp, sc->condition.data ? sc->condition.data : "", sc->condition.length);
        code_buf[code_size++] = OP_RESOLVE;
        code_buf[code_size++] = (cond_const >> 24) & 0xFF;
        code_buf[code_size++] = (cond_const >> 16) & 0xFF;
        code_buf[code_size++] = (cond_const >> 8) & 0xFF;
        code_buf[code_size++] = cond_const & 0xFF;

        uint32_t other_jobs = (total_cases_jobs - sc->then_count) + total_default_jobs;
        next_case_offset += 10 + 5 * other_jobs + 5;

        code_buf[code_size++] = OP_JUMP_IF_FALSE;
        code_buf[code_size++] = (next_case_offset >> 24) & 0xFF;
        code_buf[code_size++] = (next_case_offset >> 16) & 0xFF;
        code_buf[code_size++] = (next_case_offset >> 8) & 0xFF;
        code_buf[code_size++] = next_case_offset & 0xFF;

        SwitchCase *other_sc = job->spec.multi_switch.cases;
        while (other_sc) {
          if (other_sc != sc) {
            for (size_t b = 0; b < other_sc->then_count; b++) {
              JobNode *b_job = find_job_by_id(ast, other_sc->then_branch[b]);
              uint32_t b_const = add_constant(&cp, b_job->id.data, b_job->id.length);
              code_buf[code_size++] = OP_SKIP_JOB;
              code_buf[code_size++] = (b_const >> 24) & 0xFF;
              code_buf[code_size++] = (b_const >> 16) & 0xFF;
              code_buf[code_size++] = (b_const >> 8) & 0xFF;
              code_buf[code_size++] = b_const & 0xFF;
            }
          }
          other_sc = other_sc->next;
        }
        for (size_t b = 0; b < job->spec.multi_switch.default_count; b++) {
          JobNode *b_job = find_job_by_id(ast, job->spec.multi_switch.default_branch[b]);
          uint32_t b_const = add_constant(&cp, b_job->id.data, b_job->id.length);
          code_buf[code_size++] = OP_SKIP_JOB;
          code_buf[code_size++] = (b_const >> 24) & 0xFF;
          code_buf[code_size++] = (b_const >> 16) & 0xFF;
          code_buf[code_size++] = (b_const >> 8) & 0xFF;
          code_buf[code_size++] = b_const & 0xFF;
        }

        JobNode *first_job = find_job_by_id(ast, sc->then_branch[0]);
        int fj_idx = get_job_index(ast, first_job);
        uint32_t fj_offset = infos[fj_idx].start_offset;
        code_buf[code_size++] = OP_JUMP;
        code_buf[code_size++] = (fj_offset >> 24) & 0xFF;
        code_buf[code_size++] = (fj_offset >> 16) & 0xFF;
        code_buf[code_size++] = (fj_offset >> 8) & 0xFF;
        code_buf[code_size++] = fj_offset & 0xFF;

        sc = sc->next;
      }

      if (total_default_jobs > 0) {
        sc = job->spec.multi_switch.cases;
        while (sc) {
          for (size_t b = 0; b < sc->then_count; b++) {
            JobNode *b_job = find_job_by_id(ast, sc->then_branch[b]);
            uint32_t b_const = add_constant(&cp, b_job->id.data, b_job->id.length);
            code_buf[code_size++] = OP_SKIP_JOB;
            code_buf[code_size++] = (b_const >> 24) & 0xFF;
            code_buf[code_size++] = (b_const >> 16) & 0xFF;
            code_buf[code_size++] = (b_const >> 8) & 0xFF;
            code_buf[code_size++] = b_const & 0xFF;
          }
          sc = sc->next;
        }
      } else {
        sc = job->spec.multi_switch.cases;
        while (sc) {
          for (size_t b = 0; b < sc->then_count; b++) {
            JobNode *b_job = find_job_by_id(ast, sc->then_branch[b]);
            uint32_t b_const = add_constant(&cp, b_job->id.data, b_job->id.length);
            code_buf[code_size++] = OP_SKIP_JOB;
            code_buf[code_size++] = (b_const >> 24) & 0xFF;
            code_buf[code_size++] = (b_const >> 16) & 0xFF;
            code_buf[code_size++] = (b_const >> 8) & 0xFF;
            code_buf[code_size++] = b_const & 0xFF;
          }
          sc = sc->next;
        }
        uint32_t end_switch_offset = infos[i].start_offset + infos[i].total_size;
        code_buf[code_size++] = OP_JUMP;
        code_buf[code_size++] = (end_switch_offset >> 24) & 0xFF;
        code_buf[code_size++] = (end_switch_offset >> 16) & 0xFF;
        code_buf[code_size++] = (end_switch_offset >> 8) & 0xFF;
        code_buf[code_size++] = end_switch_offset & 0xFF;
      }
      
    } else if (job->type == NODE_FORK) {
      uint32_t job_id_const = add_constant(&cp, job->id.data ? job->id.data : "", job->id.length);
      code_buf[code_size++] = OP_COMPLETE_JOB;
      code_buf[code_size++] = (job_id_const >> 24) & 0xFF;
      code_buf[code_size++] = (job_id_const >> 16) & 0xFF;
      code_buf[code_size++] = (job_id_const >> 8) & 0xFF;
      code_buf[code_size++] = job_id_const & 0xFF;

      uint8_t count = (uint8_t)job->spec.fork_node.branch_count;
      code_buf[code_size++] = OP_FORK;
      code_buf[code_size++] = count;
      for (uint8_t b = 0; b < count; b++) {
        JobNode *branch_job = find_job_by_id(ast, job->spec.fork_node.branches[b]);
        uint32_t branch_offset = 0;
        if (branch_job) {
          int b_idx = get_job_index(ast, branch_job);
          branch_offset = infos[b_idx].start_offset;
        }
        code_buf[code_size++] = (branch_offset >> 24) & 0xFF;
        code_buf[code_size++] = (branch_offset >> 16) & 0xFF;
        code_buf[code_size++] = (branch_offset >> 8) & 0xFF;
        code_buf[code_size++] = branch_offset & 0xFF;
      }
      
      uint32_t end_fork_offset = infos[i].jump_target_offset;
      code_buf[code_size++] = OP_JUMP;
      code_buf[code_size++] = (end_fork_offset >> 24) & 0xFF;
      code_buf[code_size++] = (end_fork_offset >> 16) & 0xFF;
      code_buf[code_size++] = (end_fork_offset >> 8) & 0xFF;
      code_buf[code_size++] = end_fork_offset & 0xFF;
      
    } else if (job->type == NODE_JOIN) {
      uint8_t strat = 1;
      if (sv_equals_cstr(job->spec.join_node.strategy, "any")) strat = 2;
      else if (sv_equals_cstr(job->spec.join_node.strategy, "n_required")) strat = 3;
      
      uint32_t job_id_const = add_constant(&cp, job->id.data ? job->id.data : "", job->id.length);
      code_buf[code_size++] = OP_JOIN;
      code_buf[code_size++] = strat;
      code_buf[code_size++] = (job_id_const >> 24) & 0xFF;
      code_buf[code_size++] = (job_id_const >> 16) & 0xFF;
      code_buf[code_size++] = (job_id_const >> 8) & 0xFF;
      code_buf[code_size++] = job_id_const & 0xFF;
      
    } else {
      uint32_t id_const = add_constant(&cp, job->id.data ? job->id.data : "", job->id.length);
      code_buf[code_size++] = OP_CALL_PROVIDER;
      code_buf[code_size++] = (id_const >> 24) & 0xFF;
      code_buf[code_size++] = (id_const >> 16) & 0xFF;
      code_buf[code_size++] = (id_const >> 8) & 0xFF;
      code_buf[code_size++] = id_const & 0xFF;
    }
    
    if (infos[i].append_return) {
      code_buf[code_size++] = OP_RETURN;
    }
    if (infos[i].append_jump_end) {
      uint32_t target_offset = infos[i].jump_target_offset;
      code_buf[code_size++] = OP_JUMP;
      code_buf[code_size++] = (target_offset >> 24) & 0xFF;
      code_buf[code_size++] = (target_offset >> 16) & 0xFF;
      code_buf[code_size++] = (target_offset >> 8) & 0xFF;
      code_buf[code_size++] = target_offset & 0xFF;
    }
  }
  
  code_buf[code_size++] = OP_RETURN;

  // Add workflow name to constant pool
  uint32_t name_const_idx = add_constant(&cp, ast->name.data ? ast->name.data : "main", ast->name.length ? ast->name.length : 4);

  // Build Constant Pool binary buffer
  uint8_t const_buf[8192];
  uint32_t const_offset_bytes = 0;
  for (uint32_t i = 0; i < cp.count; i++) {
    uint32_t slen = (uint32_t)strlen(cp.strings[i]);
    const_buf[const_offset_bytes++] = (slen >> 24) & 0xFF;
    const_buf[const_offset_bytes++] = (slen >> 16) & 0xFF;
    const_buf[const_offset_bytes++] = (slen >> 8) & 0xFF;
    const_buf[const_offset_bytes++] = slen & 0xFF;
    memcpy(const_buf + const_offset_bytes, cp.strings[i], slen);
    const_offset_bytes += slen;
  }

  // Setup NVMWorkflowEntry
  NVMWorkflowEntry wf_entry;
  wf_entry.name_const_idx = name_const_idx;
  wf_entry.entry_offset = 0;
  wf_entry.metadata_offset = sizeof(NVMHeader) + const_offset_bytes + sizeof(NVMWorkflowEntry) + code_size;
  wf_entry.metadata_size = (uint32_t)ast->input_len;

  // Header setup
  NVMHeader header;
  memset(&header, 0, sizeof(NVMHeader));
  memcpy(header.magic, "NEST", 4);
  header.version = 4;
  header.flags = 1;
  header.const_pool_offset = sizeof(NVMHeader);
  header.const_pool_count = cp.count;
  header.wf_table_offset = sizeof(NVMHeader) + const_offset_bytes;
  header.wf_table_count = 1;
  header.code_offset = header.wf_table_offset + sizeof(NVMWorkflowEntry);
  header.code_size = code_size;

  // Prepare full output binary buffer to sign.
  uint32_t total_binary_size = sizeof(NVMHeader) + const_offset_bytes + sizeof(NVMWorkflowEntry) + code_size + ast->input_len;
  uint8_t *binary_buf = na_alloc(arena, total_binary_size);
  if (!binary_buf) return ERR_OOM;
  
  memcpy(binary_buf, &header, sizeof(NVMHeader));
  if (const_offset_bytes > 0) {
    memcpy(binary_buf + sizeof(NVMHeader), const_buf, const_offset_bytes);
  }
  memcpy(binary_buf + header.wf_table_offset, &wf_entry, sizeof(NVMWorkflowEntry));
  if (code_size > 0) {
    memcpy(binary_buf + header.code_offset, code_buf, code_size);
  }
  if (ast->input_len > 0) {
    memcpy(binary_buf + wf_entry.metadata_offset, ast->input_buffer, ast->input_len);
  }
  
  // Set signature block to 0 in buffer before signing
  memset(((NVMHeader *)binary_buf)->signature, 0, 64);
  
  // Compute signature over the whole binary buffer into a temporary buffer first
  // to avoid overlapping memory accesses during signature generation.
  uint8_t temp_sig[64];
  int32_t sign_res = crypto_sign_binary(DEFAULT_PRIV_KEY, binary_buf, total_binary_size, temp_sig);
  if (sign_res != 0) {
    return ERR_OOM;
  }
  memcpy(((NVMHeader *)binary_buf)->signature, temp_sig, 64);

  FILE *f = fopen(output_nbc_path, "wb");
  if (!f) return ERR_HTTP_TRANSPORT;

  fwrite(binary_buf, 1, total_binary_size, f);
  fclose(f);

  return ERR_SUCCESS;
  /*#endregion*/
}

int32_t bytecode_compile_workspace(Arena *arena, WorkspaceMap *map, const char *output_nbc_path) {
  /*#region*/
  if (!map || !map->root_workflow) return ERR_INVALID_BOUNDARY;
  int32_t compile_res = compile_workflow(arena, &map->root_workflow->ast);
  if (compile_res != ERR_SUCCESS) return compile_res;
  return bytecode_compile_workflow(arena, &map->root_workflow->ast, output_nbc_path);
  /*#endregion*/
}
