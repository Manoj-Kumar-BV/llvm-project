import os
import re
import requests
import argparse
from groq import Groq
from pdfminer.high_level import extract_text
from dotenv import load_dotenv

MODEL_NAME = "llama3-8b-8192"
PDF_PATH = "OpenMP-API-Specification-6-0.pdf"
SPEC_TEXT_CACHE = "OpenMP-API-Specification-6-0.txt"

load_dotenv()
GROQ_API_KEY = os.getenv("GROQ_API_KEY")
GITHUB_TOKEN = os.getenv("GITHUB_TOKEN")

CODE_EXTENSIONS = (
    '.c', '.cpp', '.cc', '.cxx', '.h', '.hpp', '.py', '.f90', '.f', '.F', '.F90',
    '.rs', '.java', '.js', '.ts', '.gitignore', '.python-version'
)

client = Groq(api_key=GROQ_API_KEY)

class OpenMPSpec:
    def __init__(self, pdf_path=PDF_PATH, cache_path=SPEC_TEXT_CACHE):
        self.spec_text = self._load_spec_text(pdf_path, cache_path)
        self.sections = self._index_sections()

    def _load_spec_text(self, pdf_path, cache_path):
        if os.path.exists(cache_path):
            with open(cache_path, "r", encoding="utf-8") as f:
                return f.read()
        text = extract_text(pdf_path)
        with open(cache_path, "w", encoding="utf-8") as f:
            f.write(text)
        return text

    def _index_sections(self):
        section_pattern = re.compile(r'^(?:Section\s*)?(\d+(\.\d+)*)(?:\s+)(.+)$')
        sections = []
        current_section = None
        for line in self.spec_text.split('\n'):
            m = section_pattern.match(line.strip())
            if m:
                if current_section:
                    sections.append(current_section)
                current_section = {
                    "number": m.group(1),
                    "title": m.group(3).strip(),
                    "content": []
                }
            elif current_section:
                current_section["content"].append(line)
        if current_section:
            sections.append(current_section)
        return sections

    def find_best_section(self, keywords):
        best_score = 0
        best_section = None
        for section in self.sections:
            header_text = (section["number"] + " " + section["title"]).lower()
            body_text = " ".join(section["content"]).lower()
            score = sum(3 for k in keywords if k in header_text) + sum(1 for k in keywords if k in body_text)
            if score > best_score:
                best_score = score
                best_section = section
        return best_section if best_section and best_score > 0 else None

def fetch_pr_data(repo, pr_number):
    files_url = f"https://api.github.com/repos/{repo}/pulls/{pr_number}/files"
    pr_url = f"https://api.github.com/repos/{repo}/pulls/{pr_number}"
    headers = {}
    if GITHUB_TOKEN:
        headers["Authorization"] = f"token {GITHUB_TOKEN}"
    files_res = requests.get(files_url, headers=headers)
    pr_res = requests.get(pr_url, headers=headers)
    if files_res.status_code != 200 or pr_res.status_code != 200:
        raise Exception(f"Failed to fetch PR data. Status codes: files={files_res.status_code}, pr={pr_res.status_code}")
    return files_res.json(), pr_res.json()

def extract_keywords(text):
    return set(re.findall(r'\b\w+\b', text.lower())) - {
        'a', 'an', 'the', 'in', 'of', 'to', 'and', 'is', 'for', 'int', 'float', 'if', 'else', 'return', 'void', 'bool'
    }

def summarize_patch_with_llm(filename, patch, section_text):
    prompt = f"""
You are reviewing a GitHub pull request for an OpenMP-related project.

File Changed: {filename}

Patch:
{patch}

Relevant OpenMP Spec Section:
{section_text}

Please generate a structured, reviewer-friendly summary for this change, referencing the specification where appropriate.
"""
    response = client.chat.completions.create(
        model=MODEL_NAME,
        messages=[{"role": "user", "content": prompt}],
        temperature=0.4,
        max_tokens=800,
    )
    return response.choices[0].message.content

def main():
    parser = argparse.ArgumentParser(description="OpenMP PR Summarizer (Terminal)")
    parser.add_argument('--repo', required=True, help='GitHub repository in the form owner/repo')
    parser.add_argument('--pr', required=True, help='Pull request number')
    parser.add_argument('--only-code', action='store_true', help='Summarize only code files (see CODE_EXTENSIONS)')
    parser.add_argument('--export', help='Export the result to a markdown or text file')
    args = parser.parse_args()
    
    if not GROQ_API_KEY:
        print("Error: GROQ_API_KEY not found in environment. Please check your .env file.")
        return

    try:
        print(f"Fetching PR #{args.pr} from {args.repo}...")
        patches, pr_data = fetch_pr_data(args.repo, args.pr)
        pr_title = pr_data.get("title", "No title")
        pr_url = pr_data.get("html_url", "")

        print("Loading OpenMP spec...")
        spec = OpenMPSpec()
        print(f"PR Title: {pr_title}\nURL: {pr_url}\n")

        summaries = []
        files_summarized = 0

        for i, patch_item in enumerate(patches):
            filename = patch_item.get('filename', 'unknown')
            patch = patch_item.get('patch', '')
            if not patch:
                continue
            files_summarized += 1

            if args.only_code and not filename.endswith(CODE_EXTENSIONS):
                print(f"\n--- File {i+1}: {filename} ---")
                print("Skipping LLM summary (non-code file).")
                summary = f"Non-code file `{filename}` was changed. Patch length: {len(patch) if patch else 0}.\nNo LLM summary generated."
                summary_md = f"### File: `{filename}`\n\n{summary}\n"
                print(summary + "\n" + "-"*60)
                summaries.append(summary_md)
                continue

            print(f"\n--- File {i+1}: {filename} ---")
            print("Finding relevant spec section...")

            patch_keywords = extract_keywords(patch)
            best_section = spec.find_best_section(patch_keywords)
            if best_section:
                section_info = f"Section {best_section['number']} {best_section['title']}:\n" + \
                               "\n".join(best_section['content'][:10])
            else:
                section_info = "No relevant section found"

            print("Summarizing with LLM...")
            summary = summarize_patch_with_llm(filename, patch, section_info)
            summary_md = f"### File: `{filename}`\n\n#### Relevant Spec\n{section_info}\n\n#### LLM Summary\n{summary}\n"
            print(summary + "\n" + "-"*60)
            summaries.append(summary_md)

        if files_summarized == 0:
            print("No files with patch found in this PR.")

        print("Done.")

        if args.export:
            with open(args.export, "w", encoding="utf-8") as f:
                f.write(f"# OpenMP PR Review: {pr_title}\nURL: {pr_url}\n\n" + "\n\n".join(summaries))
            print(f"Summaries exported to {args.export}")

    except Exception as e:
        print(f"\nError: {e}")

if __name__ == "__main__":
    main()