// Set namespace to the value shown by `axthena status`.
// In Neo4j Browser: :param namespace => 'your namespace'
// Execute statements individually. These inspect full runs; C++ Q1–Q7 applies
// source availability, view, hold and qualification rules.
// Expected counts below refer to the verified 48-report annual run.
// Browser's sidebar label shortcuts search all namespaces/runs. Setting a
// parameter does not filter those generated queries; use the scoped statements.

// 1. Selected published run.
MATCH (s:AxStore {id:$namespace}), (r:AxRun {key:s.latest_run})
RETURN r.id AS selected_run, r.created AS published_at;

// 2. Visual report -> expression -> idea -> organisation graph.
MATCH (s:AxStore {id:$namespace}), (r:AxRun {key:s.latest_run})
MATCH p=(report:Report {store:s.id,run:r.id})-[:CONTAINS]->(:Expression)-[:EXPRESSES]->(:Idea)-[:CONCERNS]->(:Organisation)
RETURN p LIMIT 50;

// 3. Supported company/resource relationships (inspect edge evidence/modality).
MATCH (s:AxStore {id:$namespace}), (r:AxRun {key:s.latest_run})
MATCH p=(a:Organisation {store:s.id,run:r.id})-[e:SUBSIDIARY_OF|SUPPLIES|DEPENDS_ON|PARTNERS_WITH|USES|PRODUCES]->()
RETURN p LIMIT 100;

// 4. Interpretations beside exact retained source passages.
MATCH (s:AxStore {id:$namespace}), (r:AxRun {key:s.latest_run})
MATCH (expression:Expression {store:s.id,run:r.id})
UNWIND expression.support AS passage_id
MATCH (passage:Passage {key:s.id+'/'+r.id+'/'+passage_id})
RETURN expression.name AS interpretation,passage.data_quote AS exact_passage LIMIT 25;

// 5. Record counts: Report 48, Expression 92, Idea 92, Organisation 181,
// Event 99, Claim 126, Passage 794; all kinds together total 2,977 nodes.
MATCH (s:AxStore {id:$namespace}), (r:AxRun {key:s.latest_run})
MATCH (n:AxRecord {store:s.id,run:r.id})
RETURN n.kind AS kind, count(*) AS records
ORDER BY records DESC, kind;

// 6. Monthly coverage: 24 rows, two reports for every publisher/month.
MATCH (s:AxStore {id:$namespace}), (r:AxRun {key:s.latest_run})
MATCH (report:Report {store:s.id,run:r.id})
RETURN substring(report.published_date,0,7) AS month,
       report.source_host AS publisher, count(*) AS articles
ORDER BY month, publisher;

// 7. Processing exceptions: partial #13/#14/#36/#45, insufficient #20.
// Structured failure/issue properties are retained JSON strings.
MATCH (s:AxStore {id:$namespace}), (r:AxRun {key:s.latest_run})
MATCH (report:Report {store:s.id,run:r.id})
WHERE report.data_processing_status <> 'COMPLETE'
RETURN report.data_number AS article, report.name AS title,
       report.data_processing_status AS status,
       report.data_validation_failures_json AS rejected_components,
       report.data_issues_json AS issues
ORDER BY article;

// 8. Article #17's ideas and exact supporting quotations. Change 17 to inspect
// another article. Several passages can support the same expression.
MATCH (s:AxStore {id:$namespace}), (r:AxRun {key:s.latest_run})
MATCH (report:Report {store:s.id,run:r.id,data_number:17})-[:CONTAINS]->(expression:Expression)
UNWIND expression.support AS passage_id
MATCH (passage:Passage {key:s.id+'/'+r.id+'/'+passage_id})
RETURN report.data_number AS article, expression.name AS interpretation,
       passage.data_quote AS exact_passage,
       passage.data_begin AS utf8_begin, passage.data_end AS utf8_end
ORDER BY interpretation, utf8_begin;

// 9. Claim states: REPORTED 120, CONFIRMED_SCOPED 3, COMPARISON_UNRESOLVED 3.
// Scoped confirmations concern supported announcements, not future execution.
MATCH (s:AxStore {id:$namespace}), (r:AxRun {key:s.latest_run})
MATCH (claim:Claim {store:s.id,run:r.id})
RETURN claim.data_status AS status, count(*) AS claims
ORDER BY claims DESC, status;

// 10. Meaning multiplicities: all 92 Ideas currently have one Expression and
// one publication. This sample produced no complete-meaning merges.
MATCH (s:AxStore {id:$namespace}), (r:AxRun {key:s.latest_run})
MATCH (expression:Expression {store:s.id,run:r.id})-[:EXPRESSES]->(idea:Idea)
WITH idea, count(*) AS expressions, count(DISTINCT expression.report) AS publications
RETURN expressions, publications, count(*) AS ideas
ORDER BY expressions DESC, publications DESC;

// 11. Compact company-link table. Each row groups source assertions with the
// same endpoints/type/activity/date/modality/status, retaining descriptions and
// report IDs. The counts describe supporting publications, not independent facts
// or confirmation. Physical parallel edges retain each source's own evidence.
MATCH (s:AxStore {id:$namespace}), (r:AxRun {key:s.latest_run})
MATCH (a:Organisation {store:s.id,run:r.id})
  -[e:SUBSIDIARY_OF|MEMBER_OF|PARTNERS_WITH|SUPPLIES|DEPENDS_ON|COMPETES_WITH]->(b:Organisation)
RETURN a.id AS source_id, a.name AS source_name,
       type(e) AS relationship, b.id AS target_id, b.name AS target_name,
       e.data_activity AS activity, e.data_date AS date,
       e.data_modality AS modality, e.data_assertion_status AS assertion_status,
       count(e) AS source_assertions, count(DISTINCT e.data_report) AS report_count,
       collect(DISTINCT e.data_description) AS descriptions,
       collect(DISTINCT e.data_report) AS report_ids
ORDER BY report_count DESC, source_name, relationship, target_name;

// 12. NVIDIA investigation: activity areas, reported events, source opinions and
// supported stock-price judgments. UNION branches avoid activity/opinion products.
// OPINION is an extraction classification; it does not imply UP/DOWN. As with
// the other Browser examples, this inspects the selected run's retained records.
MATCH (s:AxStore {id:$namespace}), (r:AxRun {key:s.latest_run})
MATCH (stock:Stock {store:s.id,run:r.id,symbol:'NVDA.US'})
      -[:CONFIGURED_ISSUER]->(nvidia:Organisation)
CALL {
  WITH nvidia
  MATCH path=(nvidia)-[:HAS_ACTIVITY]->(:OrganisationActivity)-[:IN_DOMAIN]->(:Domain)
  RETURN 'Activity area' AS category, path
  UNION ALL
  WITH nvidia
  MATCH path=(:Report)-[:REPORTS_EVENT]->(:Event)-[:CONCERNS]->(nvidia)
  RETURN 'Reported event' AS category, path
  UNION ALL
  WITH nvidia
  MATCH path=(:Report)-[:CONTAINS]->(:Expression {data_kind:'OPINION'})-[:CONCERNS]->(nvidia)
  RETURN 'Source opinion' AS category, path
  UNION ALL
  WITH stock
  MATCH path=(:Report)-[:CONTAINS]->(:Expression)-[:HAS_JUDGMENT]->(:Judgment)-[:TARGETS]->(stock)
  RETURN 'Stock-price judgment' AS category, path
}
RETURN category, path;
