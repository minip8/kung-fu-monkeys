#!/usr/bin/env bash
# One-time setup for the deploy workflow's `vms` job (deploy/README.md
# "Updating", .github/workflows/deploy.yml): lets GitHub Actions run
# update.sh on the VMs without a long-lived key in the repository.
#
#   deploy/gcp/setup_deploy_identity.sh --project my-proj --repo owner/name
#
# It enables the IAM Credentials, STS, IAP and OS Login APIs; creates the
# service account `kfm-deploy` with OS Login admin (sudo over SSH), the IAP
# tunnel role and Compute viewer; creates the Workload Identity Federation
# pool `github` with an OIDC provider for GitHub Actions limited to --repo,
# and lets that repository's workflows impersonate the service account;
# turns OS Login on for the project (every VM, your own `gcloud compute ssh`
# included: the Linux user becomes your account's OS Login name and sudo
# needs the OS Login admin role, which project owners have); and opens TCP 22
# to IAP's range only (35.235.240.0/20, rule `kfm-allow-iap-ssh`). It then
# prints the `gh variable set` lines for the repository. Idempotent: every
# step is skipped when its resource exists.
#
# Afterwards `default-allow-ssh` (port 22 from anywhere) can go:
#   gcloud compute firewall-rules delete default-allow-ssh --project my-proj
# and your own sessions use `gcloud compute ssh NAME --zone Z --tunnel-through-iap`.
set -euo pipefail

project=""
repo=""
pool="github"
provider="github-actions"
sa_name="kfm-deploy"
network="default"

usage() {
  cat >&2 << EOF
usage: $0 --project ID --repo OWNER/NAME [options]
  --project ID        GCP project of the VMs (required)
  --repo OWNER/NAME   the GitHub repository whose workflows may deploy (required)
  --pool NAME         workload identity pool (default: $pool)
  --provider NAME     OIDC provider in the pool (default: $provider)
  --service-account N service account name (default: $sa_name)
  --network NET       VPC network for the IAP firewall rule (default: $network)
EOF
  exit 2
}
while [[ $# -gt 0 ]]; do
  [[ $# -ge 2 ]] || usage
  case "$1" in
    --project) project="$2" ;;
    --repo) repo="$2" ;;
    --pool) pool="$2" ;;
    --provider) provider="$2" ;;
    --service-account) sa_name="$2" ;;
    --network) network="$2" ;;
    *) usage ;;
  esac
  shift 2
done
[[ -n "$project" && "$repo" == */* ]] || usage
gc=(gcloud --project "$project" --quiet)

number="$("${gc[@]}" projects describe "$project" --format='value(projectNumber)')"
sa="$sa_name@$project.iam.gserviceaccount.com"
pool_path="projects/$number/locations/global/workloadIdentityPools/$pool"

echo "== APIs"
"${gc[@]}" services enable iamcredentials.googleapis.com sts.googleapis.com \
  iap.googleapis.com oslogin.googleapis.com compute.googleapis.com

echo "== service account $sa"
if ! "${gc[@]}" iam service-accounts describe "$sa" > /dev/null 2>&1; then
  "${gc[@]}" iam service-accounts create "$sa_name" \
    --display-name "Kung Fu Monkeys deploy (GitHub Actions)"
fi
# IAM is eventually consistent: a policy binding right after the creation
# can fail with "does not exist" for up to a minute. Retry each binding.
bind_role() {
  local attempt err
  err="$(mktemp)"
  for attempt in $(seq 1 12); do
    if "${gc[@]}" projects add-iam-policy-binding "$project" \
      --member "serviceAccount:$sa" --role "$1" --condition None > /dev/null 2> "$err"; then
      rm -f "$err"
      return 0
    fi
    if ! grep -q "does not exist" "$err"; then
      cat "$err" >&2
      rm -f "$err"
      return 1
    fi
    echo "   ($sa not visible to IAM yet; retrying in 5 s)"
    sleep 5
  done
  rm -f "$err"
  echo "error: $sa still not visible to IAM after a minute" >&2
  return 1
}
for role in roles/compute.osAdminLogin roles/iap.tunnelResourceAccessor roles/compute.viewer; do
  bind_role "$role"
  echo "   $role"
done

echo "== workload identity pool $pool, provider $provider (repository $repo)"
if ! "${gc[@]}" iam workload-identity-pools describe "$pool" --location global > /dev/null 2>&1; then
  "${gc[@]}" iam workload-identity-pools create "$pool" --location global \
    --display-name "GitHub Actions"
fi
if ! "${gc[@]}" iam workload-identity-pools providers describe "$provider" \
  --workload-identity-pool "$pool" --location global > /dev/null 2>&1; then
  "${gc[@]}" iam workload-identity-pools providers create-oidc "$provider" \
    --workload-identity-pool "$pool" --location global \
    --display-name "GitHub Actions OIDC" \
    --issuer-uri "https://token.actions.githubusercontent.com" \
    --attribute-mapping "google.subject=assertion.sub,attribute.repository=assertion.repository" \
    --attribute-condition "assertion.repository == '$repo'"
fi
"${gc[@]}" iam service-accounts add-iam-policy-binding "$sa" \
  --role roles/iam.workloadIdentityUser \
  --member "principalSet://iam.googleapis.com/$pool_path/attribute.repository/$repo" > /dev/null
provider_name="$("${gc[@]}" iam workload-identity-pools providers describe "$provider" \
  --workload-identity-pool "$pool" --location global --format='value(name)')"

echo "== OS Login on for the project"
"${gc[@]}" compute project-info add-metadata --metadata enable-oslogin=TRUE > /dev/null

echo "== firewall kfm-allow-iap-ssh (TCP 22 from IAP only)"
if ! "${gc[@]}" compute firewall-rules describe kfm-allow-iap-ssh > /dev/null 2>&1; then
  "${gc[@]}" compute firewall-rules create kfm-allow-iap-ssh --network "$network" \
    --direction INGRESS --action ALLOW --rules tcp:22 --source-ranges 35.235.240.0/20 \
    --description "SSH to every VM through Identity-Aware Proxy (deploy workflow, operators)"
fi

# The project's running VMs, "name:zone ..." (the lobby VM, tag kfm-lobby, last).
vms="$("${gc[@]}" compute instances list --filter='status=RUNNING AND name~^kfm-' \
  --format='csv[no-heading](name,zone.basename(),tags.items)' \
  | awk -F, '{ line = $1 ":" $2; if ($0 ~ /kfm-lobby/) lobby = lobby " " line; else rest = rest " " line }
             END { print rest lobby }' | sed 's/^ //')"
[[ -n "$vms" ]] || vms="kfm-eu-1:europe-west1-b"
first="${vms%% *}"

cat << EOF

Done. Set the repository variables (KFM_VMS: "name:zone ..." with the lobby VM last;
this is the project's running kfm-* VMs now):

  gh variable set GCP_PROJECT_ID --body '$project'
  gh variable set GCP_WORKLOAD_IDENTITY_PROVIDER --body '$provider_name'
  gh variable set GCP_DEPLOY_SERVICE_ACCOUNT --body '$sa'
  gh variable set KFM_VMS --body '$vms'

Then the next v* tag updates the VMs from the workflow. Check the path first by hand:
  gcloud compute ssh ${first%%:*} --zone ${first#*:} --tunnel-through-iap --project $project -- sudo true
EOF
