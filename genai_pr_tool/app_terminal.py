import os
import re
import requests
from tkinter import Tk, Label, Entry, Button, messagebox, filedialog, Frame, font
from tkinter.scrolledtext import ScrolledText
from groq import Groq
from pdfminer.high_level import extract_text
from dotenv import load_dotenv

# ========== CONFIGURATION AND CONSTANTS ==========

MODEL_NAME = "llama3-8b-8192"  # Model used for summarization
PDF_PATH = "OpenMP-API-Specification-6-0.pdf"  # Path to OpenMP PDF specification
SPEC_TEXT_CACHE = "OpenMP-API-Specification-6-0.txt"  # Cached text version of the PDF

# Load environment variables (for API keys, etc.)
load_dotenv()
GROQ_API_KEY = os.getenv("GROQ_API_KEY")
GITHUB_TOKEN = os.getenv("GITHUB_TOKEN")

client = Groq(api_key=GROQ_API_KEY)

# ========== OPENMP SPECIFICATION LOADER & SEARCHER ==========

# ========== Code Extensions ==========
# Define which file extensions are considered code files (for optional filtering)
CODE_EXTENSIONS = (
    '.c', '.cpp', '.cc', '.cxx', '.h', '.hpp', '.py', '.f90', '.f', '.F', '.F90',
    '.rs', '.java', '.js', '.ts', '.gitignore', '.python-version'
)
# You can add more extensions as needed, or remove the ones you don't want to summarize
# If you want to summarize only code files, you can filter by these extensions in the summarization loop.
# ========== OpenMP Specification Class ==========

class OpenMPSpec:
    """
    Loads the OpenMP specification PDF, indexes section headers, 
    and provides keyword-based section lookup.
    """
    def __init__(self, pdf_path=PDF_PATH, cache_path=SPEC_TEXT_CACHE):
        self.spec_text = self._load_spec_text(pdf_path, cache_path)
        self.sections = self._index_sections()

    def _load_spec_text(self, pdf_path, cache_path):
        """
        Loads the specification as plain text, using a cached version if possible.
        """
        if os.path.exists(cache_path):
            with open(cache_path, "r", encoding="utf-8") as f:
                return f.read()
        text = extract_text(pdf_path)
        with open(cache_path, "w", encoding="utf-8") as f:
            f.write(text)
        return text

    def _index_sections(self):
        """
        Indexes section headers and their content from the specification text.
        """
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
        """
        Finds the most relevant section based on keyword matches.
        """
        best_score = 0
        best_section = None
        for section in self.sections:
            header_text = (section["number"] + " " + section["title"]).lower()
            body_text = " ".join(section["content"]).lower()
            # Give higher weight to keywords found in the header
            score = sum(3 for k in keywords if k in header_text) + sum(1 for k in keywords if k in body_text)
            if score > best_score:
                best_score = score
                best_section = section
        return best_section if best_section and best_score > 0 else None

# ========== GITHUB PR FETCHING ==========

def fetch_pr_data(repo, pr_number):
    """
    Fetches PR files and metadata from GitHub API.
    """
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

# ========== UTILITIES ==========

def extract_keywords(text):
    """
    Extracts keywords from text, removing common stopwords.
    """
    return set(re.findall(r'\b\w+\b', text.lower())) - {
        'a', 'an', 'the', 'in', 'of', 'to', 'and', 'is', 'for', 'int', 'float', 'if', 'else', 'return', 'void', 'bool'
    }

def summarize_patch_with_llm(filename, patch, section_text):
    """
    Calls the LLM to generate a summary for a file patch, referencing the OpenMP spec section.
    """
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

def insert_with_bold(text_widget, content):
    """
    Inserts content into text_widget, rendering **bold** segments as bold.
    """
    if "bold" not in text_widget.tag_names():
        bold_font = font.Font(text_widget, text_widget.cget("font"))
        bold_font.configure(weight="bold")
        text_widget.tag_configure("bold", font=bold_font)
    idx = 0
    for match in re.finditer(r'\*\*(.+?)\*\*', content):
        start, end = match.span()
        if start > idx:
            text_widget.insert('end', content[idx:start])
        bold_text = match.group(1)
        text_widget.insert('end', bold_text, "bold")
        idx = end
    if idx < len(content):
        text_widget.insert('end', content[idx:])

# ========== MAIN ANALYSIS FUNCTION ==========

def run_analysis():
    """
    Runs the summarization workflow: fetch PR, load spec, match sections, summarize patches, and display results.

    NOTE: By default, this script summarizes **all files** changed in the PR (including non-code files like `.gitignore`, `.python-version`, etc.)
    If you only want to summarize code files, you can add an extension check as shown in the commented block below.
    Place the "if filename.endswith(CODE_EXTENSIONS): ... else: ..." block below inside the for-loop to filter files.
    """
    repo = repo_entry.get().strip()
    pr_number = pr_entry.get().strip()
    output_text.config(state='normal')
    output_text.delete(1.0, 'end')

    if not repo or "/" not in repo or repo == repo_placeholder:
        messagebox.showerror("Input Error", "Please enter a valid repository in the format owner/repo.")
        output_text.config(state='disabled')
        return

    if not pr_number.isdigit() or pr_number == pr_placeholder:
        messagebox.showerror("Input Error", "Please enter a valid numeric PR number.")
        output_text.config(state='disabled')
        return

    if not GROQ_API_KEY:
        messagebox.showerror("Environment Error", "GROQ_API_KEY not found in environment. Please check your .env file.")
        output_text.config(state='disabled')
        return

    try:
        output_text.insert('end', f"Fetching PR #{pr_number} from {repo}...\n")
        patches, pr_data = fetch_pr_data(repo, pr_number)
        pr_title = pr_data.get("title", "No title")
        pr_url = pr_data.get("html_url", "")

        output_text.insert('end', f"Loading OpenMP spec...\n")
        spec = OpenMPSpec()
        output_text.insert('end', f"PR Title: {pr_title}\nURL: {pr_url}\n\n")

        summaries = []
        files_summarized = 0

        for i, patch_item in enumerate(patches):
            filename = patch_item.get('filename', 'unknown')
            patch = patch_item.get('patch', '')
            if not patch:
                continue
            files_summarized += 1

            # --------- START: If you only want to summarize code files, uncomment below ---------
            # if not filename.endswith(CODE_EXTENSIONS):
            #     output_text.insert('end', f"\n--- File {i+1}: {filename} ---\n")
            #     output_text.insert('end', "Skipping LLM summary (non-code file).\n")
            #     summary = f"Non-code file `{filename}` was changed. Patch length: {len(patch) if patch else 0}.\nNo LLM summary generated."
            #     summary_md = f"### File: `{filename}`\n\n{summary}\n"
            #     insert_with_bold(output_text, f"{summary}\n")
            #     output_text.insert('end', "-" * 60 + "\n")
            #     output_text.see('end')
            #     summaries.append(summary_md)
            #     continue
            # --------- END: If you only want to summarize code files, uncomment above ---------

            # By default, the summary is generated for all files
            output_text.insert('end', f"\n--- File {i+1}: {filename} ---\n")
            output_text.insert('end', "Finding relevant spec section...\n")

            patch_keywords = extract_keywords(patch)
            best_section = spec.find_best_section(patch_keywords)
            if best_section:
                section_info = f"Section {best_section['number']} {best_section['title']}:\n" + \
                               "\n".join(best_section['content'][:10])
            else:
                section_info = "No relevant section found"

            output_text.insert('end', "Summarizing with LLM...\n")
            output_text.see('end')
            summary = summarize_patch_with_llm(filename, patch, section_info)
            summary_md = f"### File: `{filename}`\n\n#### Relevant Spec\n{section_info}\n\n#### LLM Summary\n{summary}\n"

            insert_with_bold(output_text, f"{summary}\n")
            output_text.insert('end', "-" * 60 + "\n")
            output_text.see('end')
            summaries.append(summary_md)

        if files_summarized == 0:
            output_text.insert('end', "No files with patch found in this PR.\n")

        output_text.insert('end', "Done.\n")
        output_text.config(state='disabled')

        # Attach summaries to GUI for export
        root.summaries_markdown = f"# OpenMP PR Review: {pr_title}\nURL: {pr_url}\n\n" + "\n\n".join(summaries)
    except Exception as e:
        output_text.insert('end', f"\nError: {e}")
        output_text.config(state='disabled')
        messagebox.showerror("Error", str(e))

# ========== EXPORT FUNCTIONALITY ==========

def export_summaries():
    """
    Exports the generated summaries to a file (Markdown, text, etc.).
    """
    if not hasattr(root, "summaries_markdown") or not root.summaries_markdown.strip():
        messagebox.showwarning("Export", "No summaries to export. Run analysis first.")
        return
    file_path = filedialog.asksaveasfilename(
        defaultextension=".md",
        filetypes=[
            ("Markdown files", "*.md"),
            ("Text files", "*.txt"),
            ("All files", "*.*")
        ],
        title="Save summaries as..."
    )
    if not file_path:
        return
    with open(file_path, "w", encoding="utf-8") as f:
        f.write(root.summaries_markdown)
    messagebox.showinfo("Export", f"Summaries exported to {file_path}")

# ========== TKINTER GUI SETUP ==========

root = Tk()
root.title("OpenMP PR Summarizer (per-file)")
root.geometry("1050x750")

repo_placeholder = "e.g. Manoj-Kumar-BV/llvm-project"
pr_placeholder = "e.g. 123"

# Top Frame for inputs
top_frame = Frame(root)
top_frame.grid(row=0, column=0, pady=(15, 0), padx=10, sticky="ew")

Label(top_frame, text="GitHub Repository (owner/repo):").grid(row=0, column=0, padx=(0, 8), pady=5, sticky="w")

repo_entry = Entry(top_frame, width=35, fg='grey')
repo_entry.grid(row=0, column=1, padx=(0, 8), pady=5)
repo_entry.insert(0, repo_placeholder)

Label(top_frame, text="Pull Request Number:").grid(row=0, column=2, padx=(0, 8), pady=5, sticky="w")

pr_entry = Entry(top_frame, width=15, fg='grey')
pr_entry.grid(row=0, column=3, padx=(0, 8), pady=5, sticky="w")
pr_entry.insert(0, pr_placeholder)

def clear_placeholder(event, entry, placeholder):
    """Clear placeholder text on focus."""
    if entry.get() == placeholder:
        entry.delete(0, 'end')
        entry.config(fg='black')

def restore_placeholder(event, entry, placeholder):
    """Restore placeholder text if entry is empty."""
    if not entry.get():
        entry.insert(0, placeholder)
        entry.config(fg='grey')

# Bind placeholder handlers
repo_entry.bind("<FocusIn>", lambda e: clear_placeholder(e, repo_entry, repo_placeholder))
repo_entry.bind("<FocusOut>", lambda e: restore_placeholder(e, repo_entry, repo_placeholder))
pr_entry.bind("<FocusIn>", lambda e: clear_placeholder(e, pr_entry, pr_placeholder))
pr_entry.bind("<FocusOut>", lambda e: restore_placeholder(e, pr_entry, pr_placeholder))

Button(top_frame, text="Summarize", command=run_analysis, bg="lightblue", width=14).grid(row=0, column=4, padx=(5,0), pady=5)

# Output Frame
output_frame = Frame(root)
output_frame.grid(row=1, column=0, padx=10, pady=(8,0), sticky="nsew")

output_text = ScrolledText(output_frame, width=100, height=35, wrap="word")
output_text.pack(fill="both", expand=True)
output_text.config(state='disabled')

# Export Button at the bottom right (not stretched)
export_frame = Frame(root)
export_frame.grid(row=2, column=0, padx=10, pady=(8,15), sticky="e")
Button(export_frame, text="Export Summaries", command=export_summaries, bg="lightgreen", width=20).pack(anchor="e") # anchor right

# Make the output frame expandable
root.grid_rowconfigure(1, weight=1)
root.grid_columnconfigure(0, weight=1)

root.mainloop()