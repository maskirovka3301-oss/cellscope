# Windows Artifact Signing

Windows release builds are signed with Microsoft Azure Artifact Signing
(formerly Trusted Signing). The workflow uses
`Azure/artifact-signing-action@v2.0.0` and OIDC; no long-lived Azure client
secret is stored in the repository.

## Azure resources

Create or identify:

1. An **Artifact Signing account**.
2. A **certificate profile** of type Code Signing.
3. A Microsoft Entra application and service principal.
4. The endpoint for the account's region, for example
   `https://eus.codesigning.azure.net/`.

Assign the service principal the **Artifact Signing Certificate Profile
Signer** role on the Artifact Signing account (or its resource group).

## Federated identity

Add a federated credential to the Entra application with:

| Field | Value |
|---|---|
| Issuer | `https://token.actions.githubusercontent.com` |
| Subject | `repo:maskirovka3301-oss/cellscope:environment:windows-signing` |
| Audience | `api://AzureADTokenExchange` |

The workflow job uses the GitHub environment `windows-signing`. Using an
environment subject avoids per-branch federated credentials, because Azure does
not support wildcards in federated-credential subjects.

The environment can be unprotected, or protected with required reviewers or a
deployment branch policy. If protection is added, the signing job waits for
approval before Azure login.

## GitHub repository configuration

Create the environment once:

```bash
gh api --method PUT \
  repos/maskirovka3301-oss/cellscope/environments/windows-signing
```

Set repository secrets:

```bash
gh secret set AZURE_CLIENT_ID --repo maskirovka3301-oss/cellscope
gh secret set AZURE_TENANT_ID --repo maskirovka3301-oss/cellscope
gh secret set AZURE_SUBSCRIPTION_ID --repo maskirovka3301-oss/cellscope
```

Set repository variables:

```bash
gh variable set AZURE_ARTIFACT_SIGNING_ENDPOINT \
  --repo maskirovka3301-oss/cellscope
gh variable set AZURE_ARTIFACT_SIGNING_ACCOUNT_NAME \
  --repo maskirovka3301-oss/cellscope
gh variable set AZURE_ARTIFACT_SIGNING_CERTIFICATE_PROFILE_NAME \
  --repo maskirovka3301-oss/cellscope
```

The workflow fails before Azure login if any value is missing. Pull-request
builds do not request Azure credentials or sign binaries.

## What is signed

The Windows build is produced as an unsigned artifact, then the
`Sign Windows binaries` job:

1. downloads and expands `CellScope-windows-x64.zip`;
2. logs in to Azure through OIDC in the `windows-signing` environment;
3. signs every `.exe` and `.dll` in the package with SHA-256 and an RFC 3161
   Microsoft timestamp;
4. verifies every file with `Get-AuthenticodeSignature` and fails if any
   signature is not `Valid`;
5. repackages the signed zip and replaces the `cellscope-Windows` artifact.

The release job depends on this signing job, so a release build cannot publish
without valid Windows signatures.

## Verification

Download the Windows zip from the release and run:

```powershell
Get-AuthenticodeSignature .\CellScope.exe | Format-List
```

On Windows, the file Properties → Digital Signatures tab should show a valid
Microsoft Artifact Signing certificate and timestamp.
